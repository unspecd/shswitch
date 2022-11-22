## Description

This is a simple proxy server that acts as a HTTP and SOCKS proxy. It accepts incoming connections on a specified port and redirects the connections to a different ports, depending on the protocol.

## Building

Before compiling, install (libuv)[https://libuv.org/] library and its development files.

Next, build program with:
```
make
```

## Usage

Command line parameters are as follows:
```
USAGE: ./shswitch -I LOCAL_HOST -i LOCAL_PORT -H REMOTE_HTTP_HOST -h REMOTE_HTTP_PORT -S REMOTE_SOCKS_HOST -s REMOTE_SOCKS_PORT
```

For example:
```
./switch -I127.0.0.1 -i1234 -H127.0.0.1 -h3120 -S127.0.0.1 -s9050
```
