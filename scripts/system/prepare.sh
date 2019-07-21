#!/usr/bin/dash -ex
cd $GLAD
install -dv boot dev etc proc run sys usr/bin usr/cerata usr/include usr/lib usr/share var var/lib var/lib/urandom var/log
ln -fsv usr/bin bin
install -dvm 0750 root
install -v $TOOL/$TUPL/lib/libgcc_s.so.1 usr/lib
install -v $TOOL/$TUPL/lib/libstdc++.so.6.0.27 usr/lib
cd $GLAD/usr/lib
$STRIP libgcc_s.so.1 libstdc++.so.6.0.27
ln -fsv libstdc++.so.6.0.27 libstdc++.so.6
