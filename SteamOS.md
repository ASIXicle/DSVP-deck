distrobox create --name dev --image archlinux
distrobox enter dev
sudo pacman -S gcc make ffmpeg sdl2 sdl2_ttf pkgconf
git clone git@github.com:ASIXicle/DSVP.git
cd DSVP
make
./build/dsvp


OR (doesn't persist after SteamOS updates)

sudo steamos-readonly disable
sudo pacman-key --init
sudo pacman-key --populate archlinux
sudo pacman -S gcc make ffmpeg sdl2 sdl2_ttf pkgconf
git clone git@github.com:ASIXicle/DSVP.git
cd DSVP
make
./build/dsvp
