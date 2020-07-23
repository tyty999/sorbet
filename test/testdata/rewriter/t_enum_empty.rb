# typed: strict

class EmptyEnum < T::Enum
  enums do
  end
end

extend T::Sig
sig {params(x: T.nilable(EmptyEnum)).void}
def foo(x)
  case x
  # no such thing as a case without `when` branches in Ruby
  when nil then nil
  # no way to trigger the class -> union logic for sealed classes in dropSubtypesOf, because there are no subtypes
  else T.absurd(x) # error: the type `EmptyEnum` wasn't handled
  end
end
