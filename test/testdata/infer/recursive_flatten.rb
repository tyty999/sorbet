# typed: true
class A
  extend T::Sig
  extend T::Generic
  include Enumerable

  Elem = type_member(fixed: String)

  sig { override.params(blk: T.proc.params(e: Elem).void).void }
  def each(&blk)
    T.unsafe(nil)
  end

  sig { returns(T::Array[Elem])}
  def flatten
    T.unsafe(nil)
  end
end

def main
  a = A.new
  b = [A.new, A.new]

  T.reveal_type(a.flatten)
  T.reveal_type(b.flatten)
end
