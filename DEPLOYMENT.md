
sudo cmake .. -DCMAKE_BUILD_TYPE=Release -DPLATFORM=rpi5

sudo make -j$(nproc)
chmod +x /opt/boondock/airband/boondock_airband