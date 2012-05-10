# require 'profile'
require 'romp'

# client = ROMP::Client.new('tcppromp://localhost:4242', false)
# client = ROMP::Client.new('udppromp://localhost:4242', false)
client = ROMP::Client.new('unixromp:///tmp/foo', false)
obj = client.resolve("foo")

N = 10

# --- Test normal functions ---
puts "Normal functions, no synchronization"
GC.start
start_time = Time.now
for i in 1..N do
    obj.foo(i)
end
obj.sync()
total_time = Time.now - start_time
puts "  Total time: #{total_time}"
puts "  Messages per second: #{N/total_time}"

sleep(1)

# --- Test oneway functions ---
puts "Oneway functions, with synchronization"
GC.start
start_time = Time.now
for i in 1..N do
    obj.oneway_sync(:foo, i)
    # if (i % 1000) == 0 then
    #     obj.sync
    # end
end
obj.sync()
total_time = Time.now - start_time
puts "  Total time: #{total_time}"
puts "  Messages per second: #{N/total_time}"

puts "You should see the number #{N}:"
puts obj.i()

# -- Test oneway functions without sync ---
puts "Oneway functions, no synchronization"
puts "(if this is slow, it is because the TCP buffers are being filled too fast)"
GC.start
start_time = Time.now
for i in 1..N do
    obj.oneway(:foo, i)
    # if (i % 1000) == 0 then
    #     obj.sync
    # end
end
obj.sync()
total_time = Time.now - start_time
puts "  Total time: #{total_time}"
puts "  Messages per second: #{N/total_time}"

puts "You should see the number #{N}:"
puts obj.i()
# --- Test object inspection ---
puts "You should see an object Foo with an element @i=#{N}"
puts obj.inspect()

# --- Test dup ---
foo = obj.methods()
if foo.index("dup") then
    puts "uh oh, shouldn't have found dup"
end

# --- Test resopnd_to for clone ---
if obj.respond_to?("clone") then
    puts "uh oh, obj should not respond to clone"
end

# --- Test clone ---
except = false
begin
    obj.clone()
rescue NameError
    except = true
end
if !except then
    puts "uh oh, I was able to clone obj"
end

# -- Test respond_to for foo ---
if !obj.respond_to?("foo") then
    puts "uh oh, obj should respond to foo!"
end

# --- Test iterators ---
puts "You should see the numbers 1, 2, and 3 on separate lines:"
obj.each do |i|
    puts i
end

# --- Test object references ---
puts "You should see the number #{obj.i + 1}:"
b = obj.bar
puts b.i
b.release

# --- Test exceptions
puts "You should now see a RuntimeError get thrown:"
obj.throw_exception()
