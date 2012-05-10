// ROMP - The Ruby Object Message Proxy
// (C) Copyright 2001 Paul Brannan (cout at rm-f.net)
// 
// ROMP is a set of classes for providing distributed object support to a
// Ruby program.  You may distribute and/or modify it under the same terms as
// Ruby (see http://www.ruby-lang.org/en/LICENSE.txt).

#include <ruby.h>
#include <rubyio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

// ---------------------------------------------------------------------------
// Useful macros
// ---------------------------------------------------------------------------

// TODO: Are these portable?

#define PUTSHORT(s, buf) \
    do { \
        *buf = s >> 8; ++buf; \
        *buf = s & 0xff; ++buf; \
    } while(0)

#define GETSHORT(s, buf) \
    do { \
        s = (unsigned char)*buf; ++buf; \
        s = (s << 8) | (unsigned char)*buf; ++buf; \
    } while(0)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

// objects/functions created down below
//
static VALUE rb_mROMP = Qnil;
static VALUE rb_cSession = Qnil;
static VALUE rb_cProxy_Object = Qnil;
static VALUE rb_cServer = Qnil;
static VALUE rb_cObject_Reference = Qnil;
static ID id_object_id;

// objects/functions created elsewhere
//
static VALUE rb_mMarshal = Qnil;

static ID id_dump;
static ID id_load;
static ID id_message;
static ID id_backtrace;
static ID id_caller;
static ID id_raise;
static ID id_send;
static ID id_get_object;
static ID id_slice_bang;
static ID id_print_exception;
static ID id_lock;
static ID id_unlock;

static struct timeval zero_timeval;

static void init_globals() {
    rb_mMarshal = rb_const_get(rb_cObject, rb_intern("Marshal"));

    id_dump = rb_intern("dump");
    id_load = rb_intern("load");
    id_message = rb_intern("message");
    id_backtrace = rb_intern("backtrace");
    id_caller = rb_intern("caller");
    id_raise = rb_intern("raise");
    id_send = rb_intern("send");
    id_get_object = rb_intern("get_object");
    id_slice_bang = rb_intern("slice!");
    id_print_exception = rb_intern("print_exception");
    id_lock = rb_intern("lock");
    id_unlock = rb_intern("unlock");

    zero_timeval.tv_sec = 0;
    zero_timeval.tv_usec = 0;
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

// Forward declaration
static VALUE msg_to_obj(VALUE message, VALUE session, VALUE mutex);

// Create a Ruby string that won't be collected by the GC.  Be very careful
// with this function!
static void create_tmp_ruby_string(struct RString * str, char * buf, size_t len) {
    str->basic.flags = T_STRING;
    str->basic.klass = rb_cString;
    str->ptr = buf;
    str->len = len;
    str->orig = 0; // ?
}

#define WRITE_HELPER \
    do { \
        write_count = write(fd, buf, count); \
        if(write_count < 0) { \
            if(errno != EWOULDBLOCK) rb_sys_fail("write"); \
        } else if(write_count == 0 && count != 0) { \
            rb_raise(rb_eIOError, "disconnected"); \
        } else { \
            count -= write_count; \
            buf += write_count; \
            total += write_count; \
        } \
    } while(0)

#define READ_HELPER \
    do { \
        read_count = read(fd, buf, count); \
        if(read_count < 0) { \
            if(errno != EWOULDBLOCK) rb_sys_fail("read"); \
        } else if(read_count == 0 && count != 0) { \
            rb_raise(rb_eIOError, "disconnected"); \
        } else { \
            count -= read_count; \
            buf += read_count; \
            total += read_count; \
        } \
    } while(0)

// Write to an fd and raise an exception if an error occurs
static ssize_t ruby_write_throw(int fd, const void * buf, size_t count, int nonblock) {
    int n;
    size_t total = 0;
    ssize_t write_count;
    fd_set fds, error_fds;

    if(!nonblock) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_ZERO(&error_fds);
        FD_SET(fd, &error_fds);
        n = select(fd + 1, 0, &fds, &fds, 0);
        if(n > 0) {
            WRITE_HELPER;
        }
    } else {
        WRITE_HELPER;
    }

    while(count > 0) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_ZERO(&error_fds);
        FD_SET(fd, &error_fds);
        n = rb_thread_select(fd + 1, 0, &fds, &fds, 0);
        if(n == -1) {
            if(errno == EWOULDBLOCK) continue;
            rb_sys_fail("select");
        }
        WRITE_HELPER;
    };
    return total;
}

// Read from an fd and raise an exception if an error occurs
static ssize_t ruby_read_throw(int fd, void * buf, size_t count, int nonblock) {
    int n;
    size_t total = 0;
    ssize_t read_count;
    fd_set fds, error_fds;

    if(!nonblock) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_ZERO(&error_fds);
        FD_SET(fd, &error_fds);
        n = select(fd + 1, &fds, 0, &error_fds, &zero_timeval);
        if(n > 0) {
            READ_HELPER;
        }
    } else {
        READ_HELPER;
    }

    while(count > 0) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_ZERO(&error_fds);
        FD_SET(fd, &error_fds);
        n = rb_thread_select(fd + 1, &fds, 0, &error_fds, 0);
        if(n == -1) {
            if(errno == EWOULDBLOCK) continue;
            rb_sys_fail("select");
        }
        READ_HELPER;
    };

    return total;
}

// Return the message of an exception
static VALUE ruby_exc_message(VALUE exc) {
    return rb_funcall(exc, id_message, 0);
}

// Return the backtrace of an exception
static VALUE ruby_exc_backtrace(VALUE exc) {
    return rb_funcall(exc, id_backtrace, 0);
}

// Return the current Ruby call stack
static VALUE ruby_caller() {
    // TODO: Why does calling caller() with 0 arguments not work?
    return rb_funcall(rb_mKernel, id_caller, 1, INT2NUM(0));
}

// Raise a specific ruby exception with a specific message and backtrace
static void ruby_raise(VALUE exc, VALUE msg, VALUE bt) {
    rb_funcall(rb_mKernel, id_raise, 3, exc, msg, bt);
}

// Send a message to a Ruby object
static VALUE ruby_send(VALUE obj, VALUE msg) {
    return rb_apply(obj, id_send, msg);
}

// Call "get_object" on a Ruby object
static VALUE ruby_get_object(VALUE obj, VALUE object_id) {
    return rb_funcall(obj, id_get_object, 1, INT2NUM(object_id));
}

// Call slice! on a Ruby object
static VALUE ruby_slice_bang(VALUE obj, size_t min, size_t max) {
    VALUE range = rb_range_new(INT2NUM(min), INT2NUM(max), 0);
    return rb_funcall(obj, id_slice_bang, 1, range);
}

// Print an exception to the screen using a function defined in the ROMP
// module (TODO: wouldn't it be nice if Ruby's error_print were available to
// the user?)
static void ruby_print_exception(VALUE exc) {
    rb_funcall(rb_mROMP, id_print_exception, 1, exc);
}

// Call lock on an object (a mutex, probably)
static VALUE ruby_lock(VALUE obj) {
    return rb_funcall(obj, id_lock, 0);
}

// Call unlock on an object (a mutex, probably)
static VALUE ruby_unlock(VALUE obj) {
    return rb_funcall(obj, id_unlock, 0);
}

// ---------------------------------------------------------------------------
// Marshalling functions
// ---------------------------------------------------------------------------

// Take an object as input and return it as a marshalled string.
static VALUE marshal_dump(VALUE obj) {
    return rb_funcall(rb_mMarshal, id_dump, 1, obj);
}

// Take a marshalled string as input and return it as an object.
static VALUE marshal_load(VALUE str) {
    return rb_funcall(rb_mMarshal, id_load, 1, str);
}

// ---------------------------------------------------------------------------
// Session functions
// ---------------------------------------------------------------------------

#define ROMP_REQUEST           0x1001
#define ROMP_REQUEST_BLOCK     0x1002
#define ROMP_ONEWAY            0x1003
#define ROMP_ONEWAY_SYNC       0x1004
#define ROMP_RETVAL            0x2001
#define ROMP_EXCEPTION         0x2002
#define ROMP_YIELD             0x2003
#define ROMP_SYNC              0x4001
#define ROMP_NULL_MSG          0x4002
#define ROMP_MSG_START         0x4242
#define ROMP_MAX_ID            (1<<16)
#define ROMP_MAX_MSG_TYPE      (1<<16)

#define ROMP_BUFFER_SIZE       16

typedef struct {
    VALUE io_object;
    int read_fd, write_fd;
    char buf[ROMP_BUFFER_SIZE];
    int nonblock;
} ROMP_Session;

typedef uint16_t MESSAGE_TYPE_T;
typedef uint16_t OBJECT_ID_T;

// A ROMP message is broken into 3 components (see romp.rb for more details)
typedef struct {
    MESSAGE_TYPE_T message_type;
    OBJECT_ID_T object_id;
    VALUE message_obj;
} ROMP_Message;

// Send a message to the server with data data and length len.
static void send_message_helper(
        ROMP_Session * session,
        char * data,
        size_t len,
        MESSAGE_TYPE_T message_type,
        OBJECT_ID_T object_id) {

    char * buf = session->buf;

    PUTSHORT(ROMP_MSG_START,    buf);
    PUTSHORT(len,               buf);
    PUTSHORT(message_type,      buf);
    PUTSHORT(object_id,         buf);

    ruby_write_throw(session->write_fd, session->buf, ROMP_BUFFER_SIZE, session->nonblock);
    ruby_write_throw(session->write_fd, data, len, session->nonblock);
}

// Send a message to the server with the data in message.
static void send_message(ROMP_Session * session, ROMP_Message * message) {
    VALUE data;
    struct RString * data_str;

    data = marshal_dump(message->message_obj);
    data_str = RSTRING(data);
    send_message_helper(
        session,
        data_str->ptr,
        data_str->len,
        message->message_type,
        message->object_id);
}

// Send a null message to the server (no data, data len = 0)
static void send_null_message(ROMP_Session * session) {
    send_message_helper(session, "", 0, ROMP_NULL_MSG, 0);
}

// Receive a message from the server
static void get_message(ROMP_Session * session, ROMP_Message * message) {
    uint16_t magic          = 0;
    uint16_t data_len       = 0;
    char * buf              = 0;
    // struct RString message_string;
    VALUE ruby_str;

    do {
        buf = session->buf;
        
        ruby_read_throw(session->read_fd, buf, ROMP_BUFFER_SIZE, session->nonblock);

        GETSHORT(magic,                 buf);
        GETSHORT(data_len,              buf);
        GETSHORT(message->message_type, buf);
        GETSHORT(message->object_id,    buf);
    } while(magic != ROMP_MSG_START);

    buf = ALLOCA_N(char, data_len);
    ruby_read_throw(session->read_fd, buf, data_len, session->nonblock);
    // create_tmp_ruby_string(&message_string, buf, data_len);
    ruby_str = rb_str_new(buf, data_len);

    if(message->message_type != ROMP_NULL_MSG) {
        message->message_obj = marshal_load(ruby_str);
    } else {
        message->message_obj = Qnil;
    }
}

// Ideally, this function should return true if the server has disconnected,
// but currently always returns false.  The server thread will still exit
// when the client has disconnected, but currently does so via an exception.
static int session_finished(ROMP_Session * session) {
    // TODO: Detect a disconnection
    return 0;
}

// Send a sync message to the server.
static void send_sync(ROMP_Session * session) {
    ROMP_Message message = { ROMP_SYNC, 0, Qnil };
    send_message(session, &message);
}

// Wait for a sync response from the server.  Ignore any messages that are
// received while waiting for the response.
static void wait_sync(ROMP_Session * session) {
    ROMP_Message message;

    // sleep(1);
    get_message(session, &message);
    if(   message.message_type != ROMP_SYNC
       && message.object_id != 1
       && message.message_obj != Qnil) {
        rb_raise(rb_eRuntimeError, "ROMP synchronization failed");
    }
}

// Send a reply to a sync request.
static void reply_sync(ROMP_Session * session, int value) {
    if(value == 0) {
        ROMP_Message message = { ROMP_SYNC, 1, Qnil };
        send_message(session, &message);
    }
}

// ----------------------------------------------------------------------------
// Server functions
// ----------------------------------------------------------------------------

// We use this structure to pass data to our exception handler.  This is done
// by casting a pointer to a Ruby VALUE... not 100% kosher, but it should work.
typedef struct {
    ROMP_Session * session;
    ROMP_Message * message;
    VALUE obj;
    int debug;
} Server_Info;

// Make a method call into a Ruby object.
static VALUE server_funcall(VALUE ruby_server_info) {
    Server_Info * server_info = (Server_Info *)(ruby_server_info);
    return ruby_send(server_info->obj, server_info->message->message_obj);
}

// Send a yield message to the client, indicating that it should call
// Kernel#yield with the message that is sent.
static VALUE server_send_yield(VALUE retval, VALUE ruby_server_info) {
    Server_Info * server_info = (Server_Info *)(ruby_server_info);

    server_info->message->message_type = ROMP_YIELD;
    server_info->message->object_id = 0;
    server_info->message->message_obj = retval;
    send_message(server_info->session, server_info->message);

    return Qnil;
}

// Send a return value to the client, indicating that it should return
// the message to the caller.
static VALUE server_send_retval(VALUE retval, VALUE ruby_server_info) {
    Server_Info * server_info = (Server_Info *)(ruby_server_info);

    server_info->message->message_type = ROMP_RETVAL;
    server_info->message->object_id = 0;
    server_info->message->message_obj = retval;
    send_message(server_info->session, server_info->message);

    return Qnil;
}

// Send an exception the client, indicating that it should raise an exception.
static VALUE server_exception(VALUE ruby_server_info, VALUE exc) {
    Server_Info * server_info = (Server_Info *)(ruby_server_info);
    VALUE caller = ruby_caller();
    VALUE bt = ruby_exc_backtrace(exc);

    server_info->message->message_type = ROMP_EXCEPTION;
    server_info->message->object_id = 0;
    server_info->message->message_obj = exc;

    // Get rid of extraneous caller information to make debugging easier.
    ruby_slice_bang(bt, RARRAY(bt)->len - RARRAY(caller)->len - 1, -1);

    // If debugging is enabled, then print an exception.
    if(server_info->debug) {
        ruby_print_exception(exc);
    }

    send_message(server_info->session, server_info->message);

    return Qnil;
}

// Proces a request from the client and send an appropriate reply.
static VALUE server_reply(VALUE ruby_server_info) {
    Server_Info * server_info = (Server_Info *)(ruby_server_info);
    VALUE retval;
    int status;

    server_info->obj = ruby_get_object(
        server_info->obj,
        server_info->message->object_id);

    // TODO: The client should be able to pass a callback object to the server;
    // msg_to_obj can create a Proxy_Object, but it needs a session to make
    // calls over.

    // Perform the appropriate action based on message type.
    switch(server_info->message->message_type) {
        case ROMP_ONEWAY_SYNC:
            send_null_message(server_info->session);
            // fallthrough
 
        case ROMP_ONEWAY:
            rb_protect(server_funcall, ruby_server_info, &status);
            return Qnil;

        case ROMP_REQUEST:
            retval = ruby_send(
                server_info->obj,
                server_info->message->message_obj);
            break;

        case ROMP_REQUEST_BLOCK:
            retval = rb_iterate(
                server_funcall, ruby_server_info,
                server_send_yield, ruby_server_info);
            break;
 
        case ROMP_SYNC:
            reply_sync(
                server_info->session,
                server_info->message->object_id);
            return Qnil;

        default:
            rb_raise(rb_eRuntimeError, "Bad session request");
    }

    server_send_retval(retval, ruby_server_info);

    return Qnil;
}

// The main server loop.  Wait for a message from the client, route the
// message to the appropriate object, send a response and repeat.
static void server_loop(ROMP_Session * session, VALUE resolve_server, int dbg) {
    ROMP_Message message;
    Server_Info server_info = { session, &message, resolve_server, dbg };
    VALUE ruby_server_info = (VALUE)(&server_info);

    while(!session_finished(session)) {
        get_message(session, &message);
        rb_rescue2(
            server_reply, ruby_server_info,
            server_exception, ruby_server_info, rb_eException, 0);
        server_info.obj = resolve_server;
    }
}

// ----------------------------------------------------------------------------
// Client functions
// ----------------------------------------------------------------------------

// We use this structure to pass data to our client functions by casting it
// to a Ruby VALUE (see above note with Server_Info).
typedef struct {
    ROMP_Session * session;
    VALUE ruby_session;
    OBJECT_ID_T object_id;
    VALUE message;
    VALUE mutex;
} Proxy_Object;

// Send a request to the server, wait for a response, and perform an action
// based on what that response was.  This is not thread-safe, so the caller
// should perform any necessary locking
static VALUE client_request(VALUE ruby_proxy_object) {
    Proxy_Object * obj = (Proxy_Object *)(ruby_proxy_object);
    ROMP_Message msg = {
        rb_block_given_p() ? ROMP_REQUEST_BLOCK : ROMP_REQUEST,
        obj->object_id,
        obj->message
    };
    send_message(obj->session, &msg);

    for(;;) {
        get_message(obj->session, &msg);
        switch(msg.message_type) {
            case ROMP_RETVAL:
                return msg_to_obj(msg.message_obj, obj->ruby_session, obj->mutex);
                break;
            case ROMP_YIELD:
                rb_yield(msg_to_obj(msg.message_obj, obj->ruby_session, obj->mutex));
                break;
            case ROMP_EXCEPTION: {
                ruby_raise(
                    msg.message_obj,
                    ruby_exc_message(msg.message_obj),
                    rb_ary_concat(ruby_exc_backtrace(msg.message_obj), ruby_caller())
                );
                break;
            }
            case ROMP_SYNC:
                reply_sync(obj->session, NUM2INT(msg.message_obj));
                break;
            default:
                rb_raise(rb_eRuntimeError, "Invalid msg type received");
        }
    }
}

// Send a oneway message to the server.  This is not thread-safe, so the
// caller should perform any necessary locking.
static VALUE client_oneway(VALUE ruby_proxy_object) {
    Proxy_Object * obj = (Proxy_Object *)(ruby_proxy_object);
    ROMP_Message msg = {
        ROMP_ONEWAY,
        obj->object_id,
        obj->message
    };
    send_message(obj->session, &msg);
    return Qnil;
}

// Send a oneway message to the server and request a message in response.
// This is not thread-safe, so the caller should perform any necessary
// locking.
static VALUE client_oneway_sync(VALUE ruby_proxy_object) {
    Proxy_Object * obj = (Proxy_Object *)(ruby_proxy_object);
    ROMP_Message msg = {
        ROMP_ONEWAY_SYNC,
        obj->object_id,
        obj->message
    };
    send_message(obj->session, &msg);
    get_message(obj->session, &msg);
    return Qnil;
}

// Synchronize with the server.  This is not thread-safe, so the caller should
// perform any necessary locking.
static VALUE client_sync(VALUE ruby_proxy_object) {
    Proxy_Object * obj = (Proxy_Object *)(ruby_proxy_object);
    send_sync(obj->session);
    wait_sync(obj->session);
    return Qnil;
}

// ----------------------------------------------------------------------------
// Ruby interface functions
// ----------------------------------------------------------------------------

static void ruby_session_mark(ROMP_Session * session) {
    rb_gc_mark(session->io_object);
}

static VALUE ruby_session_new(VALUE self, VALUE io_object) {
    ROMP_Session * session;
    VALUE ruby_session;
    OpenFile * openfile;
    FILE * read_fp;
    FILE * write_fp;

    if(!rb_obj_is_kind_of(io_object, rb_cIO)) {
        rb_raise(rb_eTypeError, "Expecting an IO object");
    } 
    
    ruby_session = Data_Make_Struct(
        rb_cSession,
        ROMP_Session,
        (RUBY_DATA_FUNC)(ruby_session_mark),
        (RUBY_DATA_FUNC)(free),
        session);

    GetOpenFile(io_object, openfile);
    read_fp = GetReadFile(openfile);
    write_fp = GetWriteFile(openfile);
    session->read_fd = fileno(read_fp);
    session->write_fd = fileno(write_fp);
    session->io_object = io_object;
    session->nonblock = 0;

    return ruby_session;
}

static VALUE ruby_set_nonblock(VALUE self, VALUE nonblock) {
    ROMP_Session * session;
    Data_Get_Struct(self, ROMP_Session, session);
    if(nonblock == Qtrue) {
        session->nonblock = 1;
    } else if(nonblock == Qfalse) {
        session->nonblock = 0;
    } else {
        rb_raise(rb_eTypeError, "Expecting a boolean");
    }
    return Qnil;
}

static void ruby_proxy_object_mark(Proxy_Object * proxy_object) {
    rb_gc_mark(proxy_object->ruby_session);
    rb_gc_mark(proxy_object->mutex);
}

static VALUE ruby_proxy_object_new(
        VALUE self, VALUE ruby_session, VALUE ruby_mutex, VALUE ruby_object_id) {
    ROMP_Session * session;
    OBJECT_ID_T object_id = NUM2INT(ruby_object_id);
    Proxy_Object * proxy_object;
    VALUE ruby_proxy_object;

    if(!rb_obj_is_kind_of(ruby_session, rb_cSession)) {
        rb_raise(rb_eTypeError, "Expecting a session");
    }
    Data_Get_Struct(ruby_session, ROMP_Session, session);

    ruby_proxy_object = Data_Make_Struct(
        rb_cProxy_Object,
        Proxy_Object,
        (RUBY_DATA_FUNC)(ruby_proxy_object_mark),
        (RUBY_DATA_FUNC)(free),
        proxy_object);
    proxy_object->session = session;
    proxy_object->ruby_session = ruby_session;
    proxy_object->mutex = ruby_mutex;
    proxy_object->object_id = object_id;

    return ruby_proxy_object;
}

static VALUE ruby_proxy_object_method_missing(VALUE self, VALUE message) {
    Proxy_Object * proxy_object;
    Data_Get_Struct(self, Proxy_Object, proxy_object);

    proxy_object->message = message;
    ruby_lock(proxy_object->mutex);
    return rb_ensure(
        client_request, (VALUE)(proxy_object),
        ruby_unlock, proxy_object->mutex);
}

static VALUE ruby_proxy_object_oneway(VALUE self, VALUE message) {
    Proxy_Object * proxy_object;
    Data_Get_Struct(self, Proxy_Object, proxy_object);

    proxy_object->message = message;
    ruby_lock(proxy_object->mutex);
    rb_ensure(
        client_oneway, (VALUE)(proxy_object),
        ruby_unlock, proxy_object->mutex);
    return Qnil;
}

static VALUE ruby_proxy_object_oneway_sync(VALUE self, VALUE message) {
    Proxy_Object * proxy_object;
    Data_Get_Struct(self, Proxy_Object, proxy_object);

    proxy_object->message = message;
    ruby_lock(proxy_object->mutex);
    rb_ensure(
        client_oneway_sync, (VALUE)(proxy_object),
        ruby_unlock, proxy_object->mutex);
    return Qnil;
}

static VALUE ruby_proxy_object_sync(VALUE self) {
    Proxy_Object * proxy_object;
    Data_Get_Struct(self, Proxy_Object, proxy_object);

    ruby_lock(proxy_object->mutex);
    rb_ensure(
        client_sync, (VALUE)(proxy_object),
        ruby_unlock, proxy_object->mutex);
    return Qnil;
}

static VALUE ruby_server_loop(VALUE self, VALUE ruby_session) {
    ROMP_Session * session;
    VALUE resolve_server;
    VALUE ruby_debug;
    int debug;

    if(!rb_obj_is_kind_of(ruby_session, rb_cSession)) {
        rb_raise(rb_eTypeError, "Excpecting a session");
    }
    Data_Get_Struct(ruby_session, ROMP_Session, session);

    resolve_server = rb_iv_get(self, "@resolve_server");

    ruby_debug = rb_iv_get(self, "@debug");
    debug = (ruby_debug != Qfalse) && !NIL_P(ruby_debug);
    server_loop(session, resolve_server, debug);
    return Qnil;
}

// Given a message, convert it into an object that can be returned.  This
// function really only checks to see if an Object_Reference has been returned
// from the server, and creates a new Proxy_Object if this is the case.
// Otherwise, the original object is returned to the client.
static VALUE msg_to_obj(VALUE message, VALUE session, VALUE mutex) {
    if(CLASS_OF(message) == rb_cObject_Reference) {
        return ruby_proxy_object_new(
            rb_cProxy_Object,
            session,
            mutex,
            rb_funcall(message, id_object_id, 0));
    } else {
        return message;
    }
}

void Init_romp_helper() {
    init_globals();

    rb_mROMP = rb_define_module("ROMP");
    rb_cSession = rb_define_class_under(rb_mROMP, "Session", rb_cObject);

    rb_define_const(rb_cSession, "REQUEST", INT2NUM(ROMP_REQUEST));
    rb_define_const(rb_cSession, "REQUEST_BLOCK", INT2NUM(ROMP_REQUEST_BLOCK));
    rb_define_const(rb_cSession, "ONEWAY", INT2NUM(ROMP_ONEWAY));
    rb_define_const(rb_cSession, "ONEWAY_SYNC", INT2NUM(ROMP_ONEWAY_SYNC));
    rb_define_const(rb_cSession, "RETVAL", INT2NUM(ROMP_RETVAL));
    rb_define_const(rb_cSession, "EXCEPTION", INT2NUM(ROMP_EXCEPTION));
    rb_define_const(rb_cSession, "YIELD", INT2NUM(ROMP_YIELD));
    rb_define_const(rb_cSession, "SYNC", INT2NUM(ROMP_SYNC));
    rb_define_const(rb_cSession, "NULL_MSG", INT2NUM(ROMP_NULL_MSG));
    rb_define_const(rb_cSession, "MSG_START", INT2NUM(ROMP_MSG_START));
    rb_define_const(rb_cSession, "MAX_ID", INT2NUM(ROMP_MAX_ID));
    rb_define_const(rb_cSession, "MAX_MSG_TYPE", INT2NUM(ROMP_MAX_MSG_TYPE));

    rb_define_singleton_method(rb_cSession, "new", ruby_session_new, 1);
    rb_define_method(rb_cSession, "set_nonblock", ruby_set_nonblock, 1);

    rb_cProxy_Object = rb_define_class_under(rb_mROMP, "Proxy_Object", rb_cObject);
    rb_define_singleton_method(rb_cProxy_Object, "new", ruby_proxy_object_new, 3);
    rb_define_method(rb_cProxy_Object, "method_missing", ruby_proxy_object_method_missing, -2);
    rb_define_method(rb_cProxy_Object, "oneway", ruby_proxy_object_oneway, -2);
    rb_define_method(rb_cProxy_Object, "oneway_sync", ruby_proxy_object_oneway_sync, -2);
    rb_define_method(rb_cProxy_Object, "sync", ruby_proxy_object_sync, 0);

    rb_cServer = rb_define_class_under(rb_mROMP, "Server", rb_cObject);
    rb_define_method(rb_cServer, "server_loop", ruby_server_loop, 1);

    rb_cObject_Reference = rb_define_class_under(rb_mROMP, "Object_Reference", rb_cObject);

    id_object_id = rb_intern("object_id");
}
