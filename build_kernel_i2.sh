rm -rf out
mkdir out
make msm7627a_smartfren_d5c_defconfig
echo 
make -j5
cp arch/arm/boot/zImage out
echo "Build done!"
exit
