#include <vector>
#include <cerrno>

#include <iostream>
#include <fstream>
#include <cstdarg>
#include <cstring>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

#include "vm/call_frame.hpp"
#include "vm/helpers.hpp"

#include "vm/object_utils.hpp"
#include "vm/vm.hpp"

#include "compiled_file.hpp"
#include "objectmemory.hpp"
#include "global_cache.hpp"
#include "config_parser.hpp"

#include "arguments.hpp"

#include "builtin/array.hpp"
#include "builtin/exception.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/string.hpp"
#include "builtin/bignum.hpp"
#include "builtin/class.hpp"
#include "builtin/compactlookuptable.hpp"
#include "builtin/location.hpp"
#include "builtin/lookuptable.hpp"
#include "builtin/symbol.hpp"
#include "builtin/tuple.hpp"
#include "builtin/taskprobe.hpp"
#include "builtin/float.hpp"
#include "builtin/methodtable.hpp"

#include "builtin/staticscope.hpp"
#include "builtin/block_environment.hpp"

#include "builtin/system.hpp"
#include "signal.hpp"
#include "lookup_data.hpp"

#include "instruments/stats.hpp"

#include "configuration.hpp"

#include "inline_cache.hpp"

#ifdef ENABLE_LLVM
#include "llvm/jit.hpp"
#include "llvm/jit_compiler.hpp"
#endif

namespace rubinius {

  void System::attach_primitive(STATE, Module* mod, bool meta, Symbol* name, Symbol* prim) {
    MethodTable* tbl;

    if(meta) {
      tbl = mod->metaclass(state)->method_table();
    } else {
      tbl = mod->method_table();
    }

    Executable* oc = Executable::allocate(state, Qnil);
    oc->primitive(state, prim);
    oc->resolve_primitive(state);

    tbl->store(state, name, oc, G(sym_public));
  }

/* Primitives */
  //
  // HACK: remove this when performance is better and compiled_file.rb
  // unmarshal_data method works.
  Object* System::compiledfile_load(STATE, String* path, Integer* version) {
    if(!state->probe->nil_p()) {
      state->probe->load_runtime(state, std::string(path->c_str()));
    }

    std::ifstream stream(path->c_str());
    if(!stream) {
      return Primitives::failure();
    }

    CompiledFile* cf = CompiledFile::load(stream);
    if(cf->magic != "!RBIX") {
      return Primitives::failure();
    }

    uint64_t ver = version->to_ulong_long();
    if(ver > 0 && cf->version > 0 && cf->version != ver) {
      return Primitives::failure();
    }

    Object *body = cf->body(state);

    delete cf;
    return body;
  }

  Object* System::yield_gdb(STATE, Object* obj) {
    obj->show(state);
    Exception::assertion_error(state, "yield_gdb called and not caught");
    return obj;
  }

  /* @todo Improve error messages */
  Object* System::vm_exec(STATE, String* path, Array* args) {

    // Some system (darwin) don't let execvp work if there is more
    // than one thread running. So we kill off any background LLVM
    // thread here.

#ifdef ENABLE_LLVM
    LLVMState::shutdown(state);
#endif

    // TODO Need to stop and kill off any ruby threads!
    // We haven't run into this because exec is almost always called
    // after fork(), which pulls over just one thread anyway.

    std::size_t argc = args->size();

    /* execvp() requires a NULL as last element */
    std::vector<char*> argv((argc + 1), NULL);

    for (std::size_t i = 0; i < argc; ++i) {
      /* strdup should be OK. Trying to exec with strings containing NUL == bad. --rue */
      argv[i] = ::strdup(as<String>(args->get(state, i))->c_str());
    }

    // Reset all signal handlers to the defaults, so any we setup in Rubinius
    // won't leak through.
    for(int i = 0; i < NSIG; i++) {
      signal(i, SIG_DFL);
    }

    (void) ::execvp(path->c_str(), &argv[0]); /* std::vector is contiguous. --rue */

    /* execvp() returning means it failed. */
    Exception::errno_error(state, "execvp(2) failed");
    return NULL;
  }

  Object* System::vm_wait_pid(STATE, Fixnum* pid_obj, Object* no_hang) {
    pid_t input_pid = pid_obj->to_native();
    int options = 0;
    int status;
    pid_t pid;

    if(no_hang == Qtrue) {
      options |= WNOHANG;
    }

  retry:

    {
      GlobalLock::UnlockGuard lock(state->global_lock());
      pid = waitpid(input_pid, &status, options);
    }

    if(pid == -1) {
      if(errno == ECHILD) return Qfalse;
      if(errno == EINTR)  goto retry;

      // TODO handle other errnos?
      return Qfalse;
    }

    if(no_hang == Qtrue && pid == 0) {
      return Qnil;
    }

    Object* output;
    if(WIFEXITED(status)) {
      output = Fixnum::from(WEXITSTATUS(status));
    } else {
      output = Qnil;
    }

    if(input_pid > 0) {
      return output;
    }

    return Tuple::from(state, 2, output, Fixnum::from(pid));
  }

  Object* System::vm_exit(STATE, Fixnum* code) {
    state->thread_state()->raise_exit(code);
    return NULL;
  }

  Fixnum* System::vm_fork(VM* state)
  {
    int result = 0;

#ifdef ENABLE_LLVM
    LLVMState::pause(state);
#endif

    GlobalLock& lock = state->global_lock();

    // Unlock the lock here, before fork.
    if(lock.unlock() != thread::cUnlocked) { abort(); }

    // ok, now fork!
    result = ::fork();

    // Ok, now in the child, reinitialize the lock and lock it.
    // We can't lock it without reinitializing it, because it's
    // associated with the parent resources, and we're in the child!
    if(result == 0) {
      lock.init();

      // When we lock in the child, provide a little debugging so
      // we don't deadlock. There have been bugs here.
      if(lock.try_lock() == thread::cLockBusy) {
        std::cerr << "[Lock Error: GIL locking error in child]\n";

        // There is really nothing else we can do! If we call lock(),
        // we'll just block forever, there is no one else to unlock it.
        rubinius::abort();
      }

    // otherwise, we're locking in the parent, so we can just lock as normal.
    } else {
      lock.lock();
    }

    // We're in the child...
    if(result == 0) {
      /*  @todo any other re-initialisation needed? */

      state->shared.reinit();

      // Re-initialize LLVM
#ifdef ENABLE_LLVM
      LLVMState::on_fork(state);
#endif
    } else {
#ifdef ENABLE_LLVM
      LLVMState::unpause(state);
#endif
    }

    if(result == -1) {
      Exception::errno_error(state, "fork(2) failed");
      return NULL;
    }

    return Fixnum::from(result);
  }

  Object* System::vm_gc_start(STATE, Object* force) {
    // force is set if this is being called by the kernel (for instance
    // in File#ininitialize). If we decided to ignore some GC.start calls
    // by usercode trying to be clever, we can use force to know that we
    // should NOT ignore it.
    state->om->collect_young_now = true;
    state->om->collect_mature_now = true;
    state->interrupts.set_perform_gc();
    return Qnil;
  }

  Object* System::vm_get_config_item(STATE, String* var) {
    ConfigParser::Entry* ent = state->shared.user_variables.find(var->c_str());
    if(!ent) return Qnil;

    if(ent->is_number()) {
      return Fixnum::from(atoi(ent->value.c_str()));
    } else if(ent->is_true()) {
      return Qtrue;
    }

    return String::create(state, ent->value.c_str());
  }

  Object* System::vm_get_config_section(STATE, String* section) {
    ConfigParser::EntryList* list;

    list = state->shared.user_variables.get_section(
        reinterpret_cast<char*>(section->byte_address()));

    Array* ary = Array::create(state, list->size());
    for(size_t i = 0; i < list->size(); i++) {
      String* var = String::create(state, list->at(i)->variable.c_str());
      String* val = String::create(state, list->at(i)->value.c_str());

      ary->set(state, i, Tuple::from(state, 2, var, val));
    }

    delete list;

    return ary;
  }

  Object* System::vm_reset_method_cache(STATE, Symbol* name) {
    // 1. clear the global cache
    state->global_cache()->clear(name);

    state->shared.ic_registry()->clear(name);
    return name;
  }

   /*  @todo Could possibly capture the system backtrace at this
   *        point. --rue
   */
  Array* System::vm_backtrace(STATE, Fixnum* skip, CallFrame* calling_environment) {
    CallFrame* call_frame = calling_environment;

    for(native_int i = skip->to_native(); call_frame && i > 0; --i) {
      call_frame = static_cast<CallFrame*>(call_frame->previous);
    }

    Array* bt = Array::create(state, 5);

    while(call_frame) {
      // Ignore synthetic frames
      if(call_frame->cm) {
        bt->append(state, Location::create(state, call_frame));
      }

      call_frame = static_cast<CallFrame*>(call_frame->previous);
    }

    return bt;
  }

  Object* System::vm_show_backtrace(STATE, CallFrame* calling_environment) {
    calling_environment->print_backtrace(state);
    return Qnil;
  }

  Object* System::vm_profiler_instrumenter_available_p(STATE) {
#ifdef RBX_PROFILER
    return Qtrue;
#else
    return Qfalse;
#endif
  }

  Object* System::vm_profiler_instrumenter_active_p(STATE) {
    return state->shared.profiling() ? Qtrue : Qfalse;
  }

  Object* System::vm_profiler_instrumenter_start(STATE) {
    state->shared.enable_profiling(state);
    return Qtrue;
  }

  LookupTable* System::vm_profiler_instrumenter_stop(STATE) {
    return state->shared.disable_profiling(state);
  }

  Object* System::vm_write_error(STATE, String* str) {
    std::cerr << str->c_str() << std::endl;
    return Qnil;
  }

  Object* System::vm_jit_info(STATE) {
    if(state->shared.config.jit_disabled) return Qnil;

#ifdef ENABLE_LLVM
    LLVMState* ls = LLVMState::get(state);

    Array* ary = Array::create(state, 5);
    ary->set(state, 0, Integer::from(state, ls->jitted_methods()));
    ary->set(state, 1, Integer::from(state, ls->code_bytes()));
    ary->set(state, 2, Integer::from(state, ls->time_spent));
    ary->set(state, 3, Integer::from(state, ls->accessors_inlined()));
    ary->set(state, 4, Integer::from(state, ls->uncommons_taken()));

    return ary;
#else
    return Qnil;
#endif
  }

  Object* System::vm_stats_gc_clear(STATE) {
#ifdef RBX_GC_STATS
    stats::GCStats::clear();
#endif
    return Qnil;
  }

  Object* System::vm_stats_gc_info(STATE) {
#ifdef RBX_GC_STATS
    return stats::GCStats::get()->to_ruby(state);
#else
    return Qnil;
#endif
  }

  Object* System::vm_watch_signal(STATE, Fixnum* sig) {
    SignalHandler* h = state->shared.signal_handler();
    if(h) {
      h->add_signal(sig->to_native());
      return Qtrue;
    } else {
      return Qfalse;
    }
  }

  Object* System::vm_time(STATE) {
    return Integer::from(state, time(0));
  }

  Class* System::vm_open_class(STATE, Symbol* name, Object* sup, StaticScope* scope) {
    Module* under;

    if(scope->nil_p()) {
      under = G(object);
    } else {
      under = scope->module();
    }

    return vm_open_class_under(state, name, sup, under);
  }

  // HACK: Internal helper for tracking subclasses for
  // ObjectSpace.each_object(Class)
  static void add_subclass(STATE, Object* super, Class* sub) {
    Symbol* subclasses = state->symbol("@subclasses");
    Object* ivar = super->get_ivar(state, subclasses);

    Array* ary = try_as<Array>(ivar);
    if(!ary) {
      ary = Array::create(state, 1);
      ary->set(state, 0, sub);
      super->set_ivar(state, subclasses, ary);
    } else {
      ary->append(state, sub);
    }
  }

  Class* System::vm_open_class_under(STATE, Symbol* name, Object* super, Module* under) {
    bool found = false;

    Object* obj = under->get_const(state, name, &found);
    if(found) {
      Class* cls = as<Class>(obj);
      if(super->nil_p()) return cls;

      if(cls->true_superclass(state) != super) {
        std::ostringstream message;
        message << "Superclass mismatch: given "
                << as<Module>(super)->name()->c_str(state)
                << " but previously set to "
                << cls->true_superclass(state)->name()->c_str(state);

        Exception* exc =
          Exception::make_type_error(state, Class::type, super, message.str().c_str());
        // exc->locations(state, System::vm_backtrace(state, Fixnum::from(0), call_frame));
        state->thread_state()->raise_exception(exc);
        return NULL;
      }

      return cls;
    }

    // Create the class.
    if(super->nil_p()) super = G(object);
    Class* cls = Class::create(state, as<Class>(super));

    if(under == G(object)) {
      cls->name(state, name);
    } else {
      cls->set_name(state, under, name);
    }

    under->set_const(state, name, cls);

    // HACK for ObjectSpace.each_object(Class)
    add_subclass(state, super, cls);

    return cls;
  }

  Module* System::vm_open_module(STATE, Symbol* name, StaticScope* scope) {
    Module* under = G(object);

    if(!scope->nil_p()) {
      under = scope->module();
    }

    return vm_open_module_under(state, name, under);
  }

  Module* System::vm_open_module_under(STATE, Symbol* name, Module* under) {
    bool found;

    Object* obj = under->get_const(state, name, &found);

    if(found) return as<Module>(obj);

    Module* module = Module::create(state);

    module->set_name(state, under, name);
    under->set_const(state, name, module);

    return module;
  }

  Tuple* System::vm_find_method(STATE, Object* recv, Symbol* name) {
    LookupData lookup(recv, recv->lookup_begin(state));
    lookup.priv = true;

    Dispatch dis(name);

    if(!GlobalCache::resolve(state, name, dis, lookup)) {
      return (Tuple*)Qnil;
    }

    return Tuple::from(state, 2, dis.method, dis.module);
  }

  Object* System::vm_add_method(STATE, Symbol* name, CompiledMethod* method,
                                StaticScope* scope, Object* vis) {
    Module* mod = scope->for_method_definition();

    method->scope(state, scope);
    method->serial(state, Fixnum::from(0));
    mod->add_method(state, name, method);

    if(Class* cls = try_as<Class>(mod)) {
      method->formalize(state, false);

      object_type type = (object_type)cls->instance_type()->to_native();
      TypeInfo* ti = state->om->type_info[type];
      if(ti) {
        method->specialize(state, ti);
      }
    }

    vm_reset_method_cache(state, name);

    return method;
  }

  Object* System::vm_attach_method(STATE, Symbol* name, CompiledMethod* method,
                                   StaticScope* scope, Object* recv) {
    Module* mod = recv->metaclass(state);

    method->scope(state, scope);
    method->serial(state, Fixnum::from(0));
    mod->add_method(state, name, method);

    vm_reset_method_cache(state, name);

    return method;
  }

  Class* System::vm_object_class(STATE, Object* obj) {
    return obj->class_object(state);
  }

  Object* System::vm_object_metaclass(STATE, Object* obj) {
    if(obj->reference_p()) return obj->metaclass(state);
    if(obj->true_p()) return G(true_class);
    if(obj->false_p()) return G(false_class);
    if(obj->nil_p()) return G(nil_class);
    return Primitives::failure();
  }

  Object* System::vm_object_respond_to(STATE, Object* obj, Symbol* name) {
    return obj->respond_to(state, name, Qfalse);
  }

  Object* System::vm_inc_global_serial(STATE) {
    return Fixnum::from(state->shared.inc_global_serial());
  }

  Object* System::vm_jit_block(STATE, BlockEnvironment* env, Object* show) {
#ifdef ENABLE_LLVM
    LLVMState* ls = LLVMState::get(state);
    timer::Running timer(ls->time_spent);

    VMMethod* vmm = env->vmmethod(state);

    jit::Compiler jit;
    jit.compile_block(ls, env->method(), vmm);

    if(show->true_p()) {
      jit.show_machine_code();
    }
#endif

    return show;
  }

  Object* System::vm_deoptimize_inliners(STATE, Executable* exec) {
    exec->clear_inliners(state);
    return Qtrue;
  }

  Object* System::vm_raise_exception(STATE, Exception* exc) {
    state->thread_state()->raise_exception(exc);
    return NULL;
  }

  Fixnum* System::vm_memory_size(STATE, Object* obj) {
    if(obj->reference_p()) {
      size_t bytes = obj->size_in_bytes(state);
      Object* iv = obj->ivars();
      if(LookupTable* lt = try_as<LookupTable>(iv)) {
        bytes += iv->size_in_bytes(state);
        bytes += lt->values()->size_in_bytes(state);
        bytes += (lt->entries()->to_native() * sizeof(LookupTableBucket));
      } else if(iv->reference_p()) {
        bytes += iv->size_in_bytes(state);
      }
      return Fixnum::from(bytes);
    }

    return Fixnum::from(0);
  }

  Object* System::vm_throw(STATE, Symbol* dest, Object* value) {
    state->thread_state()->raise_throw(dest, value);
    return NULL;
  }

  Object* System::vm_catch(STATE, Symbol* dest, Object* obj, CallFrame* call_frame) {
    LookupData lookup(obj, obj->lookup_begin(state), false);
    Dispatch dis(state->symbol("call"));

    Arguments args;
    args.set_recv(obj);

    Object* ret = dis.send(state, call_frame, lookup, args);

    if(!ret && state->thread_state()->raise_reason() == cCatchThrow) {
      if(state->thread_state()->throw_dest() == dest) {
        Object* val = state->thread_state()->raise_value();
        state->thread_state()->clear_return();
        return val;
      }
    }

    return ret;
  }

  Object* System::vm_set_class(STATE, Object* obj, Class* cls) {
    if(!obj->reference_p()) return Primitives::failure();
    if(obj->type_id() != cls->type_info()->type)
      return Primitives::failure();

    if(kind_of<PackedObject>(obj)) {
      if(obj->klass()->packed_size() != cls->packed_size())
        return Primitives::failure();
    }

    obj->klass(state, cls);
    return obj;
  }

  Object* System::vm_method_missing_reason(STATE) {
    switch(state->method_missing_reason()) {
    case ePrivate:
      return state->symbol("private");
    case eProtected:
      return state->symbol("protected");
    case eSuper:
      return state->symbol("super");
    case eVCall:
      return state->symbol("vcall");
    case eNormal:
      return state->symbol("normal");
    default:
      return state->symbol("none");
    }
  }

  Object* System::vm_extended_modules(STATE, Object* obj) {
    if(MetaClass* mc = try_as<MetaClass>(obj->klass())) {
      Array* ary = Array::create(state, 3);

      Module* mod = mc->superclass();
      while(IncludedModule* im = try_as<IncludedModule>(mod)) {
        ary->append(state, im->module());

        mod = mod->superclass();
      }

      return ary;
    }

    return Qnil;
  }

  Symbol* System::vm_get_kcode(STATE) {
    switch(state->shared.kcode_page()) {
    case kcode::eEUC:
      return state->symbol("EUC");
    case kcode::eSJIS:
      return state->symbol("SJIS");
    case kcode::eUTF8:
      return state->symbol("UTF8");
    default:
      return state->symbol("NONE");
    }
  }

  Object* System::vm_set_kcode(STATE, String* what) {
    if(what->size() < 1) {
      kcode::set(state, kcode::eAscii);
    } else {
      const char* str = what->c_str();

      switch(str[0]) {
      case 'E':
      case 'e':
        kcode::set(state, kcode::eEUC);
        break;
      case 'S':
      case 's':
        kcode::set(state, kcode::eSJIS);
        break;
      case 'U':
      case 'u':
        kcode::set(state, kcode::eUTF8);
        break;
      default:
        kcode::set(state, kcode::eAscii);
        break;
      }
    }

    return vm_get_kcode(state);
  }

  Object* System::vm_const_defined(STATE, Symbol* sym, CallFrame* calling_environment) {
    bool found;

    Object* res = Helpers::const_get(state, calling_environment, sym, &found);
    if(!found) return Primitives::failure();

    return res;
  }

  Object* System::vm_const_defined_under(STATE, Module* under, Symbol* sym,
                                         Object* send_const_missing,
                                         CallFrame* calling_environment)
  {
    bool found;

    Object* res = Helpers::const_get_under(state, under, sym, &found);
    if(!found) {
      if(send_const_missing->true_p()) {
        res = Helpers::const_missing_under(state, under, sym, calling_environment);
      } else {
        res = Primitives::failure();
      }
    }

    return res;
  }

  Object* System::vm_check_callable(STATE, Object* obj, Symbol* sym, Object* self) {
    Module* mod = obj->lookup_begin(state);

    MethodTableBucket* entry;
    bool skip_vis_check = false;

    while(!mod->nil_p()) {
      entry = mod->method_table()->find_entry(state, sym);

      if(entry) {
        if(entry->undef_p(state)) return Qfalse;
        if(!skip_vis_check) {
          if(entry->private_p(state)) return Qfalse;
          if(entry->protected_p(state)) {
            if(!self->kind_of_p(state, mod)) return Qfalse;
          }
        }

        // It's callable, ok, but see if we should see if it's just a stub
        // to change the visibility of another method.
        if(entry->method()->nil_p()) {
          skip_vis_check = true;
        } else {
          return Qtrue;
        }
      }

      mod = mod->superclass();
    }

    return Qfalse;
  }

  Object* System::vm_check_super_callable(STATE, CallFrame* call_frame) {
    Module* mod = call_frame->module()->superclass();

    MethodTableBucket* entry;
    Symbol* sym = call_frame->original_name();

    while(!mod->nil_p()) {
      entry = mod->method_table()->find_entry(state, sym);

      if(entry) {
        if(entry->undef_p(state)) return Qfalse;

        // It's callable, ok, but see if we should see if it's just a stub
        // to change the visibility of another method.
        if(!entry->method()->nil_p()) {
          return Qtrue;
        }
      }

      mod = mod->superclass();
    }

    return Qfalse;
  }
}
