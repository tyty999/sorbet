# typed: true

module A
  class Foo
    include X
    blah do
      include Y
      Z
    end
  end
end
