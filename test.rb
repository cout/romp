def foo
    raise RuntimeError
end

Thread.new do
    begin
        foo
    rescue Exception
        puts $!, $!.backtrace
    end
end
