# frozen_string_literal: true
# typed: strict

module T::Props
  module Private
    module SerdeTransform
      extend T::Sig

      class Serialize; end
      private_constant :Serialize
      class DeserializeCloned; end
      private_constant :DeserializeCloned
      class DeserializeFrozen; end
      private_constant :DeserializeFrozen
      ModeType = T.type_alias {T.any(Serialize, DeserializeCloned, DeserializeFrozen)}
      private_constant :ModeType

      module Mode
        SERIALIZE = T.let(Serialize.new.freeze, Serialize)
        DESERIALIZE_CLONED = T.let(DeserializeCloned.new.freeze, DeserializeCloned)
        DESERIALIZE_FROZEN = T.let(DeserializeFrozen.new.freeze, DeserializeFrozen)
      end

      sig do
        params(
          type: T.any(T::Types::Base, Module),
          mode: ModeType,
          varname: String,
        )
        .returns(T.nilable(String))
        .checked(:never)
      end
      def self.generate(type, mode, varname)
        dup_or_freeze = mode == Mode::DESERIALIZE_FROZEN ? 'freeze' : 'dup'
        maybe_freeze = mode == Mode::DESERIALIZE_FROZEN ? '.freeze' : ''

        case type
        when T::Types::TypedArray, T::Types::TypedSet
          inner = generate(type.type, mode, 'v')
          if inner.nil?
            "#{varname}.#{dup_or_freeze}"
          elsif mode == Mode::DESERIALIZE_FROZEN && no_allocs?(inner)
            "#{varname}.each {|v| #{inner}}"
          elsif type.is_a?(T::Types::TypedSet)
            "Set.new(#{varname}) {|v| #{inner}}#{maybe_freeze}"
          else
            "#{varname}.map {|v| #{inner}}#{maybe_freeze}"
          end
        when T::Types::TypedHash
          keys = generate(type.keys, mode, 'k')
          values = generate(type.values, mode, 'v')
          if mode == Mode::DESERIALIZE_FROZEN && (keys || values) && no_allocs?(keys) && no_allocs?(values)
            "#{varname}.each {|k,v| #{[keys, values].compact.join('; ')}}"
          elsif keys && values
            "#{varname}.each_with_object({}) {|(k,v),h| h[#{keys}] = #{values}}#{maybe_freeze}"
          elsif keys
            "#{varname}.transform_keys {|k| #{keys}}#{maybe_freeze}"
          elsif values
            "#{varname}.transform_values {|v| #{values}}#{maybe_freeze}"
          else
            "#{varname}.#{dup_or_freeze}"
          end
        when T::Types::Simple
          raw = type.raw_type
          if raw == NilClass || raw == TrueClass || raw == FalseClass || Symbol >= raw || Numeric >= raw
            nil
          elsif String >= raw
            mode == Mode::DESERIALIZE_FROZEN ? "#{varname}.freeze" : nil
          elsif raw < T::Props::Serializable
            handle_serializable_subtype(varname, raw, mode)
          elsif raw.singleton_class < T::Props::CustomType
            handle_custom_type(varname, T.unsafe(raw), mode)
          elsif T::Configuration.scalar_types.include?(raw.name)
            # It's a bit of a hack that this is separate from NO_TRANSFORM_TYPES
            # and doesn't check inheritance (like `T::Props::CustomType.scalar_type?`
            # does), but it covers the main use case (pay-server's custom `Boolean`
            # module) without either requiring `T::Configuration.scalar_types` to
            # accept modules instead of strings (which produces load-order issues
            # and subtle behavior changes) or eating the performance cost of doing
            # an inheritance check by manually crawling a class hierarchy and doing
            # string comparisons.
            nil
          else
            handle_unknown_type(varname, mode)
          end
        when T::Types::Union
          non_nil_type = T::Utils.unwrap_nilable(type)
          if non_nil_type
            inner = generate(non_nil_type, mode, varname)
            if inner.nil?
              nil
            else
              "#{varname}.nil? ? nil : #{inner}"
            end
          else
            # Handle, e.g., T::Boolean
            if type.types.all? {|t| generate(t, mode, varname).nil?}
              nil
            else
              handle_unknown_type(varname, mode)
            end
          end
        when T::Types::Enum
          generate(T::Utils.lift_enum(type), mode, varname)
        else
          if type.singleton_class < T::Props::CustomType
            # Sometimes this comes wrapped in a T::Types::Simple and sometimes not
            handle_custom_type(varname, T.unsafe(type), mode)
          else
            handle_unknown_type(varname, mode)
          end
        end
      end

      sig {params(generated_code: T.nilable(String)).returns(T::Boolean).checked(:never)}
      private_class_method def self.no_allocs?(generated_code)
        # Special case: we know we didn't make any `from_hash`/`deserialize`
        # calls, so we can do everything in place.
        #
        # There are other cases where we could do that, but they're less
        # common and more complex and we don't bother.
        generated_code.nil? || generated_code.match?(/\A\w+\.freeze\z/)
      end

      sig {params(varname: String, type: Module, mode: ModeType).returns(String).checked(:never)}
      private_class_method def self.handle_serializable_subtype(varname, type, mode)
        case mode
        when Serialize
          "#{varname}.serialize(strict)"
        when DeserializeCloned, DeserializeFrozen
          type_name = T.must(module_name(type))

          # Check arity for compatibility with cases where `from_hash` has been
          # overridden without support for `opts` (or even the old `strict` arg)
          if T::Utils.arity(type.method(:from_hash)).abs >= 2
            "#{type_name}.from_hash(#{varname}, opts)"
          elsif mode == Mode::DESERIALIZE_FROZEN
            "T::Props::Utils.deep_freeze_object!(#{type_name}.from_hash(#{varname}))"
          else
            "#{type_name}.from_hash(#{varname})"
          end
        else
          T.absurd(mode)
        end
      end

      sig {params(varname: String, type: Module, mode: ModeType).returns(String).checked(:never)}
      private_class_method def self.handle_custom_type(varname, type, mode)
        case mode
        when Serialize
          type_name = T.must(module_name(type))
          "T::Props::CustomType.checked_serialize(#{type_name}, #{varname})"
        when DeserializeCloned
          type_name = T.must(module_name(type))
          "#{type_name}.deserialize(#{varname})"
        when DeserializeFrozen
          type_name = T.must(module_name(type))
          "T::Props::Utils.deep_freeze_object!(#{type_name}.deserialize(#{varname}))"
        else
          T.absurd(mode)
        end
      end

      sig {params(varname: String, mode: ModeType).returns(String).checked(:never)}
      private_class_method def self.handle_unknown_type(varname, mode)
        case mode
        when Serialize, DeserializeCloned
          "T::Props::Utils.deep_clone_object(#{varname})"
        when DeserializeFrozen
          "T::Props::Utils.deep_freeze_object!(#{varname})"
        else
          T.absurd(mode)
        end
      end

      # Guard against overrides of `name` or `to_s`
      MODULE_NAME = T.let(Module.instance_method(:name), UnboundMethod)
      private_constant :MODULE_NAME

      sig {params(type: Module).returns(T.nilable(String)).checked(:never)}
      private_class_method def self.module_name(type)
        MODULE_NAME.bind(type).call
      end
    end
  end
end
