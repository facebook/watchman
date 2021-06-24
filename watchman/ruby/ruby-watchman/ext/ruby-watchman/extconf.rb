# Copyright (c) 2014-present Facebook. All Rights Reserved.

require 'mkmf'

def header(item)
  unless find_header(item)
    puts "couldn't find #{item} (required)"
    exit 1
  end
end

# mandatory headers
header('ruby.h')
header('fcntl.h')
header('sys/errno.h')
header('sys/socket.h')

# variable headers
have_header('ruby/st.h') # >= 1.9; sets HAVE_RUBY_ST_H
have_header('st.h')      # 1.8; sets HAVE_ST_H

RbConfig::MAKEFILE_CONFIG['CC'] = ENV['CC'] if ENV['CC']

create_makefile('ruby-watchman/ext')
