romp
====

ROMP is the Ruby Object Message Proxy. It is sort of like drb
(distributed Ruby) in that it allows a Ruby client program to
transparently talk to an object that is sitting on a server. Its
features include:

  * Up to 40000 messages per second
  * Allows more than one object to reside on the server; supports a sort
    for naming service for retriving references to objects.
  * Fully thread-safe, provided the object sitting on the server is
    thread-safe.
  * Supports oneway calls, so the client does not have to block waiting
    for a response from the server.
  * Allows exceptions to be propogated from server to client; massages
    the backtrace to make debugging easier.

NEWS (Feb. 21, 2002) - The author of drb has done some performance tests
comparing ROMP and drb. It looks like drb is a LOT faster than it used
to be (but still not as fast as ROMP). You can see the results here:

http://www.jin.gr.jp/~nahi/RWiki/?cmd=view;name=ROMP%3A%3A%C2%AE%C5%D9%C8%E6%B3%D3 (Japanese)

