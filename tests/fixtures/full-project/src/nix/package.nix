{ lib
, stdenv
, fetchurl
, cmake
, pkg-config
, sqlite
, pcre2
, universal-ctags
}:

stdenv.mkDerivation rec {
  pname = "toktoken";
  version = "0.2.0";

  src = fetchurl {
    url = "https://github.com/example/toktoken/archive/v${version}.tar.gz";
    sha256 = "0000000000000000000000000000000000000000000000000000";
  };

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

  buildInputs = [
    sqlite
    pcre2
    universal-ctags
  ];

  cmakeFlags = [
    "-DBUILD_TESTS=OFF"
    "-DCMAKE_BUILD_TYPE=Release"
  ];

  meta = with lib; {
    description = "Fast codebase indexer for AI-assisted development";
    homepage = "https://github.com/example/toktoken";
    license = licenses.agpl3Plus;
    maintainers = [ maintainers.example ];
    platforms = platforms.unix;
  };
}
