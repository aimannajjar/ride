{
  pkgs,
  ...
}:

{
  packages = [
    # tools
    pkgs.cmake
    pkgs.gnumake
    pkgs.ninja
    pkgs.bear
    pkgs.b3sum
    pkgs.jq
    pkgs.perf-tools
    pkgs.perf
    pkgs.cppcheck

    # libs
    pkgs.libbpf
    pkgs.liburing
    pkgs.jemalloc
    pkgs.libclang.lib

    # needed to compile bpf programs
    pkgs.linuxHeaders
    pkgs.llvmPackages.clang-unwrapped # wrapped uses switches incompatible with bpf

    # pki
    pkgs.openssl
  ];

  languages.c.enable = true;
  languages.rust.enable = true;

  scripts.gen-certs.exec = ''
    mkdir -p pki
    openssl req -x509 -newkey rsa:2048 -nodes \
      -keyout pki/key.pem -out pki/cert.pem -days 365 \
      -subj "/CN=localhost" \
      -addext "subjectAltName=DNS:localhost,DNS:example.com,IP:127.0.0.1"
  '';

  env = {
    LIBBPF = "${pkgs.libbpf}/include";
    LINUX = "${pkgs.linuxHeaders}/include";
    LIBCLANG_PATH = "${pkgs.libclang.lib}/lib";
    BINDGEN_EXTRA_CLANG_ARGS = "-I${pkgs.glibc.dev}/include";
  };
}
