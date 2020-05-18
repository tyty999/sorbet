module Foo
  dsl_required :foo, Integer
  dsl_required :bar, Class, :lazy
  dsl_required :baz, String
end

class SomeOtherClass; end

class Bar
  include Foo
  foo 5
  bar {SomeOtherClass}
  baz "this"
  baz "that"
  baz "the other"
end
