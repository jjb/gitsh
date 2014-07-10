require 'thread'
require 'gitsh/line_editor'
require 'gitsh/module_delegator'

class FakeLineEditor < ModuleDelegator
  def initialize
    @prompt_queue = Queue.new
    @input_read, @input_write = IO.pipe
    super(Gitsh::LineEditor)
  end

  def readline(prompt, add_to_history)
    p :setting_input
    module_delegator_target.input = input_read
    p :setting_output
    module_delegator_target.output = output_file
    p :clearing_queue
    prompt_queue.clear
    p :pushing_to_queue
    prompt_queue << prompt
    p :calling_readline
    module_delegator_target.readline(prompt, add_to_history)
  end

  def type(string)
    puts "TYPING! #{string}"
    input_write << "#{string}\n"
  end

  def send_eof
    input_write.close
  end

  def prompt
    prompt_queue.pop
  end

  private

  attr_reader :prompt_queue, :input_read, :input_write

  def output_file
    if ENV['DEBUG']
      $stdout
    else
      File.open(Tempfile.new('line_editor_out').path, 'w')
    end
  end
end
