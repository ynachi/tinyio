import os
import re

replacements = [
    (r'\bSocketAddress::V4\b', 'SocketAddress::v4'),
    (r'\bSocketAddress::V6\b', 'SocketAddress::v6'),
    (r'\bV4\(', 'v4('),
    (r'\bV6\(', 'v6('),
    (r'\bGetIp\(', 'get_ip('),
    (r'\bGetPort\(', 'get_port('),
    (r'\bResolveIp\(', 'resolve_ip('),
    (r'\bApply\(', 'apply('),
    (r'\bGet\(\)', 'get()'),
    (r'\bGetMutable\(\)', 'get_mutable()'),
    (r'\bIsValid\(\)', 'is_valid()'),
    (r'\bRelease\(\)', 'release()'),
    (r'\bSetOption\(', 'set_option('),
    (r'\bSetOptions\(', 'set_options('),
    (r'\bTcpListener::Bind\(', 'TcpListener::bind('),
    (r'\bBind\(', 'bind(')
]

files_to_update = [
    "include/tinyio/net.hpp",
    "src/tinyio/net.cc",
    "include/tinyio/fd.hpp",
    "include/tinyio/tcp_listener.hpp",
    "src/tinyio/tcp_listener.cc",
    "src/tinyio/runtime.cc",
    "examples/tinyio/03_http_services.cpp",
    "benchmarks/cpp/kio_fiber_http_bench.cpp",
    "benchmarks/cpp/kio_http_bench.cpp",
    "benchmarks/cpp/tinyio_dispatch_bench.cpp",
    "benchmarks/cpp/tinyio_http_bench.cpp",
    "readme.md"
]

for filepath in files_to_update:
    if not os.path.exists(filepath):
        continue
    with open(filepath, 'r') as f:
        content = f.read()
        
    original = content
    for old, new in replacements:
        content = re.sub(old, new, content)
        
    if content != original:
        with open(filepath, 'w') as f:
            f.write(content)
        print(f"Updated {filepath}")

