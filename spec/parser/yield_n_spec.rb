def test_case
{"RawParseTree"=>[:yield, [:array, [:lit, 42], [:lit, 24]]],
 "Ruby"=>"yield(42, 24)",
 "ParseTree"=>s(:yield, s(:lit, 42), s(:lit, 24))}
end