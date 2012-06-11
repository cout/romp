spec = Gem::Specification.new do |s|
  s.name = 'romp'
  s.version = '0.2.0'
  s.summary = 'Ruby Object Message Proxy'
  s.homepage = 'http://github.com/cout/romp/'
  s.extensions = 'ext/extconf.rb'
  s.author = 'Paul Brannan'
  s.email = 'curlypaul924@gmail.com'
  s.description = <<-END_DESCRIPTION
ROMP is the Ruby Object Message Proxy. It is sort of like drb
(distributed Ruby) in that it allows a Ruby client program to
transparently talk to an object that is sitting on a server.
  END_DESCRIPTION

  s.files = [
    'lib/romp.rb',
    'ext/romp_helper.c',
    'ext/extconf.rb',
    'sample/client.rb',
    'sample/server.rb',
    ]

  s.extra_rdoc_files = 'README.md'
end

