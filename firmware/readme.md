sudo apt install libusb-1.0-0-dev
sudo apt install sdcc


install stm8flash mac


## Mac Setup, Tested on M1 Air with Big Sur 11.4
brew install sdcc
brew install pkg-config
brew install libusb

cd /tmp
git clone git@github.com:vdudouyt/stm8flash.git
cd stm8flash
make
sudo make install
