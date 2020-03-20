# frozen_string_literal: true
# typed: true

require 'benchmark'

require_relative '../lib/sorbet-runtime'

module SorbetBenchmarks
  module Clone

    def self.run
      input = {}

      100_000.times do
        T::Props::Utils.deep_clone_object(input)
      end

      result = Benchmark.measure do
        1_000_000.times do
          T::Props::Utils.deep_clone_object(input)
        end
      end

      puts "T::Props::Utils.deep_clone_object, trivial input (μs/iter):"
      puts result

      input = {
        'prop1' => 0,
        'prop2' => 'foo',
        'prop3' => false,
        'prop4' => [1, 2, 3],
        'prop5' => {'foo' => 1, 'bar' => 2},
        'prop6' => {'prop' => ''},
        'prop7' => [{'prop' => ''}, {'prop' => ''}],
        'prop8' => {'foo' => {'prop' => ''}, 'bar' => {'prop' => ''}},
        'prop9' => Object.new,
        'prop10' => Object,
        'prop11' => [:foo, 'bar', 0.1, true, nil],
      }

      1_000.times do
        T::Props::Utils.deep_clone_object(input)
      end

      result = Benchmark.measure do
        100_000.times do
          T::Props::Utils.deep_clone_object(input)
        end
      end

      puts "T::Props::Utils.deep_clone_object, complex input (ms/100 iter):"
      puts result
    end
  end
end

