# frozen_string_literal: true
# typed: true

module T::Props::Utils
  extend T::Sig

  # Deep copy an object. The object must consist of Ruby primitive
  # types and Hashes and Arrays.
  def self.deep_clone_object(what, freeze: false)
    result = case what
    when true
      true
    when false
      false
    when Symbol, NilClass, Numeric
      what
    when Array
      what.map {|v| deep_clone_object(v, freeze: freeze)}
    when Hash
      h = what.class.new
      what.each do |k, v|
        k.freeze if freeze
        h[k] = deep_clone_object(v, freeze: freeze)
      end
      h
    when Regexp
      what.dup
    when T::Enum
      what
    else
      what.clone
    end
    freeze ? result.freeze : result
  end

  # Freeze a given object, and return all sub-items that also need freezing
  sig {params(todo: T::Array[T.untyped], obj: T.untyped).void.checked(:never)}
  private_class_method def self.freeze_one(todo, obj)
    case obj
    when Module
      # Skip freezing modules/classes, they're very different
    when Array, Struct
      obj.freeze
      # You can't concat a struct, but it has each so you can keep array/struct handling the same
      obj.each do |value|
        todo << value
      end
    when Hash
      obj.freeze
      obj.each do |key, value|
        todo << key
        todo << value
      end
    when Range
      obj.freeze
      todo << obj.begin
      todo << obj.end
    else
      # Default to just freezing all instance variables
      obj.freeze
      obj.instance_variables.each do |iv|
        todo << obj.instance_variable_get(iv) # rubocop:disable PrisonGuard/NoLurkyInstanceVariableAccess
      end
    end
  end

  sig do
    type_parameters(:T)
    .params(obj: T.type_parameter(:T))
    .returns(T.type_parameter(:T))
    .checked(:never)
  end
  def self.deep_freeze_object!(obj)
    todo = [T.unsafe(obj)]
    seen = {}

    until todo.empty?
      o = todo.pop

      case o
      when NilClass, TrueClass, FalseClass
        # don't need to be frozen
        nil
      # Short circuit on common classes.
      # Dispatch on one class, so that the compiler has separate inline caches per function call
      when Symbol
        o.freeze
      when Numeric
        o.freeze
      when String
        o.freeze
      else
        # Skip if we've already seen this object
        if !seen[o.object_id]
          seen[o.object_id] = true
          freeze_one(todo, o)
        end
      end
    end

    obj
  end

  # The prop_rules indicate whether we should check for reading a nil value for the prop/field.
  # This is mostly for the compatibility check that we allow existing documents carry some nil prop/field.
  def self.need_nil_read_check?(prop_rules)
    # . :on_load allows nil read, but we need to check for the read for future writes
    prop_rules[:optional] == :on_load || prop_rules[:raise_on_nil_write]
  end

  # The prop_rules indicate whether we should check for writing a nil value for the prop/field.
  def self.need_nil_write_check?(prop_rules)
    need_nil_read_check?(prop_rules) || T::Props::Utils.required_prop?(prop_rules)
  end

  def self.required_prop?(prop_rules)
    # Clients should never reference :_tnilable as the implementation can change.
    !prop_rules[:_tnilable]
  end

  def self.optional_prop?(prop_rules)
    # Clients should never reference :_tnilable as the implementation can change.
    !!prop_rules[:_tnilable]
  end

  def self.merge_serialized_optional_rule(prop_rules)
    {'_tnilable' => true}.merge(prop_rules.merge('_tnilable' => true))
  end
end
