require 'socket'
require 'thread'
require 'fcntl'
require 'romp_helper'

##
# ROMP - The Ruby Object Message Proxy
# @author Paul Brannan
# @version 0.1
# (C) Copyright 2001 Paul Brannan (cout at rm-f.net)
# 
# <pre>
# ROMP is a set of classes for providing distributed object support to a
# Ruby program.  You may distribute and/or modify it under the same terms as
# Ruby (see http://www.ruby-lang.org/en/LICENSE.txt).  Example:
# 
# Client 
# ------
# client = ROMP::Client.new('localhost', 4242)
# obj = client.resolve("foo")
# puts obj.foo("foo")
# obj.oneway(:foo, "foo")
# 
# Server
# ------
# class Foo
#     def foo(x); return x; end
# end
# obj = Foo.new
# server = ROMP::Server.new('localhost', 4242)
# server.bind(obj, "foo")
# server.thread.join
# 
# You can do all sorts of cool things with ROMP, including passing blocks to
# the functions, throwing exceptions and propogating them from server to
# client, and more.  Unlike CORBA, where you must create an interface
# definition and strictly adhere to it, ROMP uses marshalling, so you can
# use almost any object with it.  But, alas, it is not as powerful as CORBA.
# 
# On a fast machine, you should expect around 7000 messages per second with
# normal method calls, up to 10000 messages per second with oneway calls with
# sync, and up to 40000 messages per second for oneway calls without sync.
# These numbers can vary depending on various factors, so YMMV.
# 
# The ROMP message format is broken into 3 components:
#     [ msg_type, obj_id, message ]
# For each msg_type, there is a specific format to the message.  Additionally,
# certain msg_types are only valid when being sent to the server, and others
# are valid only when being sent back to the client.  Here is a summary:
# 
# msg_type         send to     meaning of obj_id       msg format
# ----------------------------------------------------------------------------
# REQUEST          server      obj to talk to          [:method, *args]
# REQUEST_BLOCK    server      obj to talk to          [:method, *args]
# ONEWAY           server      obj to talk to          [:method, *args]
# ONEWAY_SYNC      server      obj to talk to          [:method, *args] 
# RETVAL           client      always 0                retval
# EXCEPTION        client      always 0                $!
# YIELD            client      always 0                [value, value, ...]
# SYNC             either      0=request, 1=response   nil
# NULL_MSG         either      always 0                n/a
# 
# BUGS:
# - On a 2.2 kernel, oneway calls without sync is very slow.
# - UDP support does not currently work.
# </pre>

module ROMP

public

    ##
    # The ROMP server class.  Like its drb equivalent, this class spawns off
    # a new thread which processes requests, allowing the server to do other
    # things while it is doing processing for a distributed object.  This
    # means, though, that all objects used with ROMP must be thread-safe.
    # 
    class Server

    public
        attr_reader :obj, :thread

        ##
        # Start a ROMP server.
        #
        # @param endpoint An endpoint for the server to listen on; should be specified in URI notation.
        # @param acceptor A proc object that can accept or reject connections; it should take a Socket as an argument and returns true or false.
        # @param debug Turns on debugging messages if enabled.
        # 
        def initialize(endpoint, acceptor=nil, debug=false)
            @mutex = Mutex.new
            @debug = debug
            @resolve_server = Resolve_Server.new
            @resolve_obj = Resolve_Obj.new(@resolve_server)
            @resolve_server.register(@resolve_obj)

            @thread = Thread.new do
                server = Generic_Server.new(endpoint)
                while(socket = server.accept)
                    puts "Got a connection" if @debug
                    if acceptor then
                        if !acceptor.call(socket) then
                            socket.close
                            next
                        end
                    end
                    puts "Accepted the connection" if @debug
                    session = Session.new(socket)
                    session.set_nonblock(true)
                    Thread.new(socket) do |socket|
                        Thread.current.abort_on_exception = true
                        begin
                            # TODO: Send a sync message to the client so it
                            # knows we are ready to receive data.
                            server_loop(session)
                        rescue Exception
                            ROMP::print_exception($!) if @debug
                        end
                        puts "Connection closed" if @debug
                    end
                end
            end
        end

        ##
        # Register an object with the server.  The object will be given an
        # id of @next_id, and @next_id will be incremented.  We could use the
        # object's real id, but this is insecure.  The supplied object must
        # be thread-safe.
        #
        # @param obj The object to register.
        #
        # @return A new Object_Reference that should be returned to the client.
        #
        def create_reference(obj)
            @mutex.synchronize do
                id = @resolve_server.register(obj)
                Object_Reference.new(id) #return
            end
        end

        ##
        # Find an object in linear time and unregister it. Be careful with
        # this function, because the client may not know the object has
        # gone away.
        #
        # @param obj The object to unregister.
        #
        def delete_reference(obj)
            @mutex.synchronize do
                @resolve_server.unregister(obj)
            end
            nil #return
        end

        ##
        # Register an object with the server and bind it to name.
        #
        # @param obj The object to bind.
        # @param name The name of to bind the object to.
        #
        def bind(obj, name)
            id = @resolve_server.register(obj)
            @resolve_server.bind(name, id)
            nil #return
        end

        ##
        # This keeps the client from seeing our objects when they call inspect
        #
        alias_method :__inspect__, :inspect
        def inspect()
            return ""
        end

    private
        if false then # the following functions are implemented in C:

        ##
        # The server_loop function is the guts of the server.  It takes in
        # requests from the client and forwards them to already-registered
        # objects.
        #
        # @param session The session to run the loop with.
        #
        def server_loop(session)
        end

        end # if false
    end

    ##
    # The ROMP client class.  A ROMP server must be started on the given
    # host and port before instantiating a ROMP client.
    #
    class Client

        ##
        # Connect to a ROMP server
        #
        # @param endpoint The endpoint the server is listening on.
        # @param sync Specifies whether to synchronize between threads; turn this off to get a 20% performance boost.
        #
        def initialize(endpoint, sync=true)
            @server = Generic_Client.new(endpoint)
            @session = Session.new(@server)
            @session.set_nonblock(true)
            @mutex = sync ? Mutex.new : Null_Mutex.new
            @resolve_obj = Proxy_Object.new(@session, @mutex, 0)
        end

        ##
        # Given a string, return a proxy object that will forward requests
        # for an object on the server with that name.
        #
        # @param object_name The name of the object to resolve.
        #
        # @return A Proxy_Object that can be used to make method calls on the object in the server.
        #
        def resolve(object_name)
            @mutex.synchronize do
                object_id = @resolve_obj.resolve(object_name)
                return Proxy_Object.new(@session, @mutex, object_id)
            end
        end
    end

private

    ##
    # In case the user does not want synchronization.
    #
    class Null_Mutex
        def synchronize
            yield
        end

        def lock
        end

        def unlock
        end
    end

    ##
    # All the special functions we have to keep track of
    #
    class Functions
        GOOD = [
            :inspect, :class_variables, :instance_eval, :instance_variables,
            :to_a, :to_s
        ]

        BAD = [
            :clone, :dup, :display
        ]

        METHOD = [
            :methods, :private_methods, :protected_methods, :public_methods,
            :singleton_methods
        ]

        RESPOND = [
            [ :method,      "raise NameError" ],
            [ :respond_to?, "false" ]
        ]
    end

    ##
    # A ROMP::Object_Reference is created on the server side to represent an
    # object in the system.  It can be returned from a server object to a
    # client object, at which point it is converted into a ROMP::Proxy_Object.
    #
    class Object_Reference
        attr_reader :object_id

        def initialize(object_id)
            @object_id = object_id
        end
    end

    ##
    # A ROMP::Object acts as a proxy; it forwards most methods to the server
    # for execution.  When you make calls to a ROMP server, you will be
    # making the calls through a Proxy_Object.
    #
    class Proxy_Object

        if false then # the following functions are implemented in C:

        ##
        # The method_missing function is called for any method that is not
        # defined on the client side.  It will forward requests to the server
        # for processing, and can iterate through a block, raise an exception,
        # or return a value.
        #
        def method_missing(function, *args)
        end

        ##
        # The oneway function is called to make a oneway call to the server
        # without synchronization.
        #
        def onweway(function, *args)
        end

        ##
        # The oneway_sync function is called to make a oneway call to the
        # server with synchronization (the server will return a null message
        # to the client before it begins processing).  This is slightly safer
        # than a normal oneway call, but it is slower (except on a linux 2.2
        # kernel; see the bug list above).
        #
        def oneway_sync(function, *args)
        end
        
        ##
        # The sync function will synchronize with the server.  It sends a sync
        # request and waits for a response.
        #
        def sync()
        end

        end # if false

        # Make sure certain methods get passed down the wire.
        Functions::GOOD.each do |method|
            eval %{
                def #{method}(*args)
                      method_missing(:#{method}, *args) #return
                end
            }
        end

        # And make sure others never get called.
        Functions::BAD.each do |method|
            eval %{
                def #{method}(*args)
                    raise(NameError,
                        "undefined method `#{method}' for " +
                        "\#<#{self.class}:#{self.id}>")
                end
            }
        end

        # And remove these function names from any method lists that get
        # returned; there's nothing we can do about people who decide to
        # return them from other functions.
        Functions::METHOD.each do |method|
            eval %{
                def #{method}(*args)
                    retval = method_missing(:#{method}, *args)
                    retval.each do |item|
                        Functions::BAD.each do |bad|
                            retval.delete(bad.to_s)
                        end
                    end
                    retval #return
                end
            }
        end

        # Same here, except don't let the call go through in the first place.
        Functions::RESPOND.each do |method, action|
            eval %{
                def #{method}(arg, *args)
                    Functions::BAD.each do |bad|
                        if arg === bad.to_s then
                            return eval("#{action}")
                        end
                    end
                    method_missing(:#{method}, arg, *args) #return
                end
            }
        end

    end

    ##
    # The Resolve_Server class registers objects for the server.  You will
    # never have to use this class directly.
    #
    class Resolve_Server
        def initialize
            @next_id = 0
            @unused_ids = Array.new
            @id_to_object = Hash.new
            @name_to_id = Hash.new
        end

        def register(obj)
            if @next_id >= Session::MAX_ID then
                if @unused_ids.size == 0 then
                    raise "Object limit exceeded"
                else
                    id = @unused_ids.pop
                end
            end
            @id_to_object[@next_id] = obj
            old_id = @next_id
            @next_id = @next_id.succ()
            old_id #return
        end

        def get_object(object_id)
            @id_to_object[object_id] #return
        end

        def unregister(obj)
            delete_obj_from_array_private(@id_to_object, obj)
        end

        def bind(name, id)
            @name_to_id[name] = id
        end

        def resolve(name)
            @name_to_id[name] #return
        end

        def delete_obj_from_array_private(array, obj)
            index = array.index(obj)
            array[index] = nil unless index == nil
        end
    end

    ##
    # The Resolve_Obj class handles resolve requests for the client.  It is
    # a special ROMP object with an object id of 0.  You will never have to
    # make calls on it directly, but will instead make calls on it through
    # the Client object.
    #
    class Resolve_Obj
        def initialize(resolve_server)
            @resolve_server = resolve_server
        end

        def resolve(name)
            @resolve_server.resolve(name) #return
        end
    end

    ##
    # A Generic_Server creates an endpoint to listen on, waits for connections,
    # and accepts them when requested.  It can operate on different kinds of
    # connections.  You will never have to use this object directly.
    #
    class Generic_Server
        def initialize(endpoint)
            case endpoint
                when %r{^(tcp)?romp://(.*?):(.*)}
                    @type = "tcp"
                    @host = $2 == "" ? nil : $2
                    @port = $3
                    @server = TCPServer.new(@host, @port)
                when %r{^(udp)romp://(.*?):(.*)}
                    @type = "udp"
                    @host = $2 == "" ? nil : $2
                    @port = $3
                    @server = UDPSocket.open()
                    @server.bind(@host, @port)
                    @mutex = Mutex.new
                when %r{^(unix)romp://(.*)}
                    @type = "unix"
                    @path = $2
                    @server = UNIXServer.open(@path)
                else
                    raise ArgumentError, "Invalid endpoint"
            end
        end
        def accept
            case @type
                when "tcp"
                    socket = @server.accept
                    socket.setsockopt(Socket::SOL_TCP, Socket::TCP_NODELAY, 1)
                    socket.fcntl(Fcntl::F_SETFL, Fcntl::O_NONBLOCK)
                    socket.sync = true
                    socket #return
                when "udp"
                    @mutex.lock
                    socket = @server
                    socket.fcntl(Fcntl::F_SETFL, Fcntl::O_NONBLOCK)
                    socket #return
                when "unix"
                    socket = @server.accept
                    socket.fcntl(Fcntl::F_SETFL, Fcntl::O_NONBLOCK)
                    socket.sync = true
                    socket #return
            end
        end
    end

    ##
    # A Generic_Client connects to a Generic_Server on a given endpoint.
    # You will never have to use this object directly.
    #
    class Generic_Client
        def self.new(endpoint)
            case endpoint
                when %r{^(tcp)?romp://(.*?):(.*)}
                    socket = TCPSocket.open($2, $3)
                    socket.sync = true
                    socket.setsockopt(Socket::SOL_TCP, Socket::TCP_NODELAY, 1)
                    socket.fcntl(Fcntl::F_SETFL, Fcntl::O_NONBLOCK)
                    socket #return
                when %r{^(udp)romp://(.*?):(.*)}
                    socket = UDPSocket.open
                    socket.connect($2, $3)
                    socket #return
                when %r{^(unix)romp://(.*)}
                    socket = UNIXSocket.open($2)
                else
                    raise ArgumentError, "Invalid endpoint"
            end
        end
    end


    ##
    # Print an exception to the screen.  This is necessary, because Ruby does
    # not give us access to its error_print function from within Ruby.
    #
    # @param exc The exception object to print.
    #
    def self.print_exception(exc)
        first = true
        $!.backtrace.each do |bt|
            if first then
                puts "#{bt}: #{$!} (#{$!.message})"
            else
                puts "\tfrom #{bt}"
            end
            first = false
        end
    end

    if false then # the following classes are implemented in C:

    ##
    # The Sesssion class is defined in romp_helper.so.  You should never have
    # to use it directly.
    #
    class Session
    end

    end # if false

end
