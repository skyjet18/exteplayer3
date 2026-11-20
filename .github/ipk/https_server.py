#!/usr/bin/env python3
"""Serves a directory over HTTPS on 127.0.0.1 (self-signed certificate).
usage: https_server.py <directory> <cert+key.pem> <port>"""
import http.server
import os
import ssl
import sys

os.chdir(sys.argv[1])
server = http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[3])), http.server.SimpleHTTPRequestHandler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[2])
server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
