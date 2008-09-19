#include "vm.hpp"

#include "builtin/class.hpp"
#include "builtin/symbol.hpp"
#include "builtin/task.hpp"

#include "builtin/nativemethodcontext.hpp"


namespace rubinius {


/* Handle */


  /* Instance methods */

  Handle::Handle() : my_index(0), my_storage(NULL)
  {
    /* Nada */
  }

  Handle::Handle(const Handle& other) : my_index(other.my_index), my_storage(other.my_storage) {}

  Object* Handle::object()
  {
    return (*my_storage)[my_index];
  }



/* NativeMethodContext */


  /**
   *  Currently active NativeMethodContext access.
   *
   *  TODO: use thread-local.
   */
  static NativeMethodContext* hidden_current_native_context = NULL;


  /* Class methods */

  void NativeMethodContext::register_class_with(VM* state)
  {
    state->globals.nativectx.set(state->new_class("NativeMethodContext", state->globals.methctx.get()));
    state->globals.nativectx.get()->set_object_type(state, NContextType);
  }

  NativeMethodContext* NativeMethodContext::create(VM* state,
                                                   Message* msg = NULL,
                                                   Task* task = as<Task>(Qnil),
                                                   NativeMethod* method = reinterpret_cast<NativeMethod*>(Qnil)) /* TODO: wtf? */
  {
    static std::size_t context_size = sizeof(NativeMethodContext) + DEFAULT_STACK_SIZE;

    NativeMethodContext* nmc = static_cast<NativeMethodContext*>(state->new_struct(G(nativectx), context_size));

    /* MethodContext stuff. */
    nmc->sender(state, task->active());
    nmc->home(state, const_cast<NativeMethodContext*>(nmc));
    nmc->self(state, msg->recv);
    nmc->module(state, msg->module);
    nmc->name(state, as<Object>(msg->name));

    /* Instead of storing the memory within as MethodContexts, we heap-allocate. */
    nmc->stack_size = DEFAULT_STACK_SIZE;

    nmc->my_stack = static_cast<void*>(new char[nmc->stack_size]);

    nmc->my_action = ORIGINAL_CALL;
    nmc->my_message = msg;
    nmc->my_method = method;
    nmc->my_handles = new HandleStorage();
    nmc->my_return_value = as<Object>(Qnil);
    nmc->my_state = state;
    nmc->my_task = task;

    /* TODO: Make active here. */

    current_context_is(nmc);
    return nmc;
  }

  NativeMethodContext* NativeMethodContext::allocate(VM* state)
  {
    return create(state);
  }

  void NativeMethodContext::current_context_is(NativeMethodContext* context)
  {
    hidden_current_native_context = context;
  }

  NativeMethodContext* NativeMethodContext::current()
  {
     return hidden_current_native_context;
  }


  /* Instance methods */

}
