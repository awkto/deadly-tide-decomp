set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -q && apt-get install -yq openjdk-21-jdk-headless unzip ffmpeg python3-pip python3-venv rsync p7zip-full xvfb >/dev/null
cd /opt && curl -sL -o g.zip $(curl -s https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/latest | grep -o "https://[^\"]*ghidra_[^\"]*\.zip" | head -1) && unzip -q g.zip && rm g.zip && ln -sfn /opt/ghidra_* /opt/ghidra
echo SETUP_DONE
