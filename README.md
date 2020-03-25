Build:
a. Cross Compile for Android:
   1. Download & Extract Android NDK, for example, to ~/Android/Ndk
   2. ~/Android/Ndk/ndk-build -C ./
   3. The executables are generated to folder "libs"

b. Build for Linux host:
   make

Run:
1. As a socket server: 
./dlc_server -o 

2. As a http server:
./dlc_server

Test:
1. To test connection with DMC:
nc 127.0.0.1 3000

2. To test connection with HMI (forwarded by DMC):
echo "    {\"func_id\":1}" | nc 127.0.0.1 3001  

