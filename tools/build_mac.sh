#!/bin/bash
# Build script for MIB Studio Tools (macOS)
# Builds hdf5_export_app.app and mib_reanalyse_hdf5 into tools/dist/

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo -e "${GREEN}Building MIB Studio Tools for macOS...${NC}"

CLEAN=false
CREATE_DMG=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean) CLEAN=true; shift ;;
        --dmg) CREATE_DMG=true; shift ;;
        *) echo "Unknown option: $1"; echo "Usage: $0 [--clean] [--dmg]"; exit 1 ;;
    esac
done

if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning previous builds...${NC}"
    rm -rf build dist
fi

echo -e "${CYAN}Checking Python installation...${NC}"
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}ERROR: Python 3 not found. Please install Python 3.8 or later.${NC}"
    exit 1
fi
echo -e "${GREEN}Found: $(python3 --version)${NC}"

VENV_PATH=".venv"
if [ ! -d "$VENV_PATH" ]; then
    echo -e "${CYAN}Creating virtual environment...${NC}"
    python3 -m venv "$VENV_PATH"
fi

echo -e "${CYAN}Activating virtual environment...${NC}"
source "$VENV_PATH/bin/activate"

pip install --upgrade pip
pip install -r ../env/requirements-tools-runtime.txt
pip install -r ../env/requirements-tools-build.txt

mkdir -p dist build

echo -e "${CYAN}Building hdf5_export_app...${NC}"
pyinstaller hdf5_export_app/hdf5_export.spec --clean --workpath build --distpath dist
echo -e "${CYAN}Building mib_reanalyse_hdf5...${NC}"
pyinstaller reanalyse_hdf5/reanalyse_hdf5.spec --clean --workpath build --distpath dist

echo -e "${GREEN}Build successful!${NC}"
echo -e "Output: $SCRIPT_DIR/dist/"
[ -d "dist/hdf5_export_app.app" ] && echo -e "${CYAN}  hdf5_export_app.app$(du -sh dist/hdf5_export_app.app 2>/dev/null | cut -f1)${NC}"
[ -f "dist/mib_reanalyse_hdf5" ] && echo -e "${CYAN}  mib_reanalyse_hdf5$(du -sh dist/mib_reanalyse_hdf5 2>/dev/null | cut -f1)${NC}"

if [ "$CREATE_DMG" = true ] && [ -d "dist/hdf5_export_app.app" ]; then
    echo -e "${CYAN}Creating DMG...${NC}"
    DMG_PATH="dist/MIB_Studio_Tools.dmg"
    [ -f "$DMG_PATH" ] && rm -f "$DMG_PATH"
    hdiutil create -volname "MIB Studio Tools" -srcfolder dist -ov -format UDZO "$DMG_PATH" 2>/dev/null || true
    [ -f "$DMG_PATH" ] && echo -e "${GREEN}DMG: $DMG_PATH${NC}"
fi

echo -e "${GREEN}Done!${NC}"
