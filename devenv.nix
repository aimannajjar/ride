{
  pkgs,
  ...
}:

{
  packages = [
    pkgs.cmake
    pkgs.gnumake
    pkgs.ninja
    pkgs.bear
    pkgs.b3sum

    pkgs.libbpf
    pkgs.liburing

    # needed to compile bpf programs
    pkgs.linuxHeaders
    pkgs.llvmPackages.clang-unwrapped # wrapped uses switches incompatible with bpf

  ];

  languages.c.enable = true;
  languages.rust.enable = true;

  env = {
    LIBBPF = "${pkgs.libbpf}/include";
    LINUX = "${pkgs.linuxHeaders}/include";
  };
}
