require 'romp-rpc'

class Bar
    attr_reader :i
    
    def initialize(i, romp)
        @i = i
        @romp = romp
    end
    
    def release()
        @romp.delete_reference(self)
    end
end

class Foo < Bar
    # Initialize @i to 0
    def initialize(romp)
        super(0, romp)
        @romp = romp
    end

    # Set @i
    def foo(i)
        @i = i
    end

    # Return a reference to a new Bar object with Bar.i = @i + 1
    def bar()
        b = Bar.new(@i + 1, @romp)
        obj = @romp.create_reference(b)
        return obj
    end

    # Test iteration
    def each()
        yield 1
        yield 2
        yield 3
    end

    def throw_exception2()
        raise RuntimeError
    end

    # Test exception
    def throw_exception()
        throw_exception2()
    end
end

if ARGV.size > 1 or ARGV[0] == "-h" or ARGV[0] == '-H' then
  puts <<END
Usage: #{$0} <url>
Example urls:
  tcpromp://localhost:4242
  udpromp://localhost:4242
  unixromp:///tmp/foo
END
end

url = ARGV[0] || "tcpromp://localhost:4242"
server = ROMP::Server.new(url, false)

f = Foo.new(server)
server.bind(f, "foo")
server.thread.join
