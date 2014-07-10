require 'readline'
require './ext/gitsh/line_editor'

#line_editor = Readline
line_editor = Gitsh::LineEditor

read, write = IO.pipe
line_editor.input = read
write << "hello world\n"

t = Thread.new do
  puts 'in the readline thread'
  p read: line_editor.readline('> ', true)
  p read: line_editor.readline('> ', true)
end

sleep 1
puts 'back in the main thread'

write.flush
write.close
p leftover: read.read

t.kill
