# This works?

* `brew install readline automake`
* `brew link readline --force`
* `RUBY_CONFIGURE_OPTS="--with-readline-dir=$(brew --prefix readline)" rbenv install 3.3.5`
* `./autogen.sh`
* `bundle update`
* `RUBY=$(which ruby) ./configure`
* `make`

Don't try `make install`, it will put a broken binary into
/usr/local/bin

Don't try to be clever and make a symlink from /usr/local/bin/gitsh to your build,
the relative paths won't work.

* add this to a shell config file, perhaps .profile_local. Modify the path
to point to where you checked out and build gitsh:
  `PATH="$PATH:/Users/YOURUSERNAME/src/gitsh/bin/"`

* may need/want to clear out old installation(s):
  `sudo rm -rf  /usr/local/bin/gitsh /usr/local/share/gitsh`
