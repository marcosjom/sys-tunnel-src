#clean
rm -r tmp bin
#make
make tunnel-server  NB_LIB_TUNNEL=1 NB_LIB_SSL_SYSTEM=1 NB_LIB_Z_SYSTEM=1 NB_LIB_LZ4_SYSTEM=1
#install
mv bin/make/release/Linux/unknown/cc/tunnel-server ../sys-tunnel-res