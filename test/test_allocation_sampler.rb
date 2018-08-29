require 'minitest/autorun'
require 'allocation_sampler'

class TestAllocationSampler < Minitest::Test
  def test_initialize
    assert ObjectSpace::AllocationSampler.new
  end

  def test_init_with_params
    as = ObjectSpace::AllocationSampler.new(interval: 10)
    assert_equal 10, as.interval
  end

  def test_init_with_location
    iseq = RubyVM::InstructionSequence.new <<-eoruby
    Object.new
    Object.new
    eoruby
    as = ObjectSpace::AllocationSampler.new(interval: 1)
    as.enable
    iseq.eval
    as.disable

    assert_equal({"Object"=>{"<compiled>"=>{1=>1, 2=>1}}}, filter(as.result))
  end

  def test_location_same_line
    iseq = RubyVM::InstructionSequence.new <<-eoruby
    10.times { Object.new }
    eoruby
    as = ObjectSpace::AllocationSampler.new(interval: 1)
    as.enable
    iseq.eval
    as.disable

    assert_equal({"Object"=>{"<compiled>"=>{1=>10}}}, filter(as.result))
  end

  def test_location_mixed
    iseq = RubyVM::InstructionSequence.new <<-eoruby
    10.times { Object.new }
    Object.new
    eoruby
    as = ObjectSpace::AllocationSampler.new(interval: 1)
    as.enable
    iseq.eval
    as.disable

    assert_equal({"Object"=>{"<compiled>"=>{1=>10, 2=>1}}}, filter(as.result))
  end

  def test_location_from_method
    iseq = RubyVM::InstructionSequence.new <<-eoruby
    def foo
      10.times { Object.new }
      Object.new
    end
    foo
    eoruby
    as = ObjectSpace::AllocationSampler.new(interval: 1)
    as.enable
    iseq.eval
    as.disable

    assert_equal({"Object"=>{"<compiled>"=>{2=>10, 3=>1}}}, filter(as.result))
  end

  def test_location_larger_interval
    iseq = RubyVM::InstructionSequence.new <<-eom
    100.times { Object.new }
    100.times { Object.new }
    eom
    as = ObjectSpace::AllocationSampler.new(interval: 10)
    as.enable
    iseq.eval
    as.disable

    assert_equal({"Object"=>{"<compiled>"=>{1=>10, 2=>10}}}, filter(as.result))
    assert_equal 201, as.allocation_count
  end

  def test_interval_default
    as = ObjectSpace::AllocationSampler.new
    assert_equal 1, as.interval
  end

  def test_two_with_same_type
    as = ObjectSpace::AllocationSampler.new
    as.enable
    Object.new
    Object.new
    as.disable

    assert_equal(2, filter(as.result)[Object.name].values.flat_map(&:values).inject(:+))
  end

  def test_two_with_same_type_same_line
    as = ObjectSpace::AllocationSampler.new
    as.enable
    Object.new; Object.new
    Object.new; Object.new
    as.disable

    assert_equal(4, filter(as.result)[Object.name].values.flat_map(&:values).inject(:+))
  end

  class X
  end

  def test_expands
    as = ObjectSpace::AllocationSampler.new
    as.enable
    500.times do
      Object.new
      X.new
    end
    Object.new
    as.disable

    assert_equal(501, filter(as.result)[Object.name].values.flat_map(&:values).inject(:+))
    assert_equal(500, filter(as.result)[TestAllocationSampler::X.name].values.flat_map(&:values).inject(:+))
  end

  def d
    Object.new
  end
  def c;  5.times { d }; end
  def b;  5.times { c }; end
  def a;  5.times { b }; end

  def test_stack_trace
    as = ObjectSpace::AllocationSampler.new
    buffer = StringIO.new
    stack_printer = ObjectSpace::AllocationSampler::Display::Stack.new(
      output: buffer,
      max_depth: 4
    )
    as.enable
    a
    as.disable
    as.heaviest_types_by_file_and_line.each do |count, class_name, file, line, frames|
      stack_printer.show frames
    end
    assert_equal <<-eoout, buffer.string
TestAllocationSampler#d                                125 (100.0%)         125 (100.0%)
`-- TestAllocationSampler#c                            125 (100.0%)           0   (0.0%)
    `-- TestAllocationSampler#b                        125 (100.0%)           0   (0.0%)
        `-- TestAllocationSampler#a                    125 (100.0%)           0   (0.0%)
            `-- TestAllocationSampler#test_stack_trace 125 (100.0%)           0   (0.0%)
    eoout
  end

  private

  def filter result
    result.each_with_object({}) do |(k, top_frames), a|
      file_table = a[k] ||= {}

      top_frames.each do |top_frame_info|
        top_frame = top_frame_info[:frames][top_frame_info[:root]]
        line_table = file_table[top_frame[:file]] ||= {}
        top_frame[:lines].each do |line, (_, count)|
          line_table[line] = count
        end
      end
    end
  end
end
