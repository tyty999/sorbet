# frozen_string_literal: true
require_relative '../../test_helper'

class Opus::Types::Test::Props::UtilsTest < Critic::Unit::UnitTest
  describe 'deep_clone_object' do
    it 'works for boolean' do
      assert_equal(true, T::Props::Utils.deep_clone_object(true))
      assert_equal(false, T::Props::Utils.deep_clone_object(false))
    end

    it 'works for nil' do
      assert_equal(nil, T::Props::Utils.deep_clone_object(nil))
    end

    it 'works for string' do
      input = 'foo'
      output = T::Props::Utils.deep_clone_object(input)
      assert_equal(input, output)
      refute_equal(input.object_id, output.object_id)
    end

    it 'works for array' do
      input = [1]
      output = T::Props::Utils.deep_clone_object(input)
      assert_equal(input, output)
      refute_equal(input.object_id, output.object_id)
    end


    it 'works for hash' do
      input = {foo: 'bar'}
      output = T::Props::Utils.deep_clone_object(input)
      assert_equal(input, output)
      refute_equal(input.object_id, output.object_id)
    end

    it 'works for set' do
      input = Set[1.0]
      output = T::Props::Utils.deep_clone_object(input)
      assert_equal(input, output)
      refute_equal(input.object_id, output.object_id)
    end
  end
end
