#!/bin/bash
# Build script for Unix HDF5 Export GUI Application
# Builds a standalone app on macOS and executable on Linux.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Get script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo -e "${GREEN}Building HDF5 Export GUI Application for Unix...${NC}"

# Parse arguments
CLEAN=false
CREATE_DMG=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN=true
            shift
            ;;
        --dmg)
            CREATE_DMG=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--clean] [--dmg]"
            exit 1
            ;;
    esac
done

# Clean previous builds if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning previous builds...${NC}"
    rm -rf build dist __pycache__ *.spec.dist
fi

# Detect OS for platform-specific packaging behavior
OS_NAME="$(uname -s)"
IS_MAC=false
IS_LINUX=false
if [ "$OS_NAME" = "Darwin" ]; then
    IS_MAC=true
elif [ "$OS_NAME" = "Linux" ]; then
    IS_LINUX=true
else
    echo -e "${YELLOW}Warning: Unrecognized OS '$OS_NAME'. Proceeding with generic Unix settings.${NC}"
fi

# Check for Python
echo -e "${CYAN}Checking Python installation...${NC}"
if command -v python3 &> /dev/null; then
    SYSTEM_PYTHON="$(command -v python3)"
elif command -v python &> /dev/null; then
    SYSTEM_PYTHON="$(command -v python)"
else
    echo -e "${RED}ERROR: Python 3 not found. Please install Python 3.8 or later.${NC}"
    exit 1
fi

PYTHON_VERSION=$("$SYSTEM_PYTHON" --version)
echo -e "${GREEN}Found: $PYTHON_VERSION${NC}"

# Create virtual environment if possible; fallback to system Python if venv creation is unavailable.
VENV_PATH=".venv"
PYTHON_EXE="$SYSTEM_PYTHON"
USE_VENV=false
if [ -d "$VENV_PATH" ] && [ -x "$VENV_PATH/bin/python" ]; then
    if "$VENV_PATH/bin/python" -m pip --version >/dev/null 2>&1; then
        PYTHON_EXE="$VENV_PATH/bin/python"
        USE_VENV=true
        echo -e "${CYAN}Using existing virtual environment: $VENV_PATH${NC}"
    else
        echo -e "${YELLOW}Warning: Existing virtual environment is incomplete (pip missing). Falling back to system Python.${NC}"
    fi
elif [ ! -d "$VENV_PATH" ]; then
    echo -e "${CYAN}Creating virtual environment...${NC}"
    if "$SYSTEM_PYTHON" -m venv "$VENV_PATH"; then
        if "$VENV_PATH/bin/python" -m pip --version >/dev/null 2>&1; then
            PYTHON_EXE="$VENV_PATH/bin/python"
            USE_VENV=true
        else
            echo -e "${YELLOW}Warning: Created virtual environment is missing pip. Falling back to system Python.${NC}"
        fi
    else
        echo -e "${YELLOW}Warning: Failed to create virtual environment. Falling back to system Python.${NC}"
    fi
fi

# Check whether we are running inside a usable virtual environment.
IN_VENV="$USE_VENV"

# Upgrade pip only in venv to avoid distro-managed pip conflicts on Linux.
if [ "$IN_VENV" = true ]; then
    echo -e "${CYAN}Upgrading pip...${NC}"
    "$PYTHON_EXE" -m pip install --upgrade pip
else
    echo -e "${YELLOW}Using system Python; skipping pip self-upgrade.${NC}"
fi

# Install dependencies
echo -e "${CYAN}Installing dependencies...${NC}"
if [ "$IN_VENV" = true ]; then
    "$PYTHON_EXE" -m pip install -r ../env/requirements-scripts.txt
else
    if ! "$PYTHON_EXE" -m pip install --user -r ../env/requirements-scripts.txt; then
        echo -e "${YELLOW}Warning: --user install failed, retrying with --break-system-packages.${NC}"
        "$PYTHON_EXE" -m pip install --break-system-packages -r ../env/requirements-scripts.txt
    fi
fi

# Build with PyInstaller
echo -e "${CYAN}Building application bundle with PyInstaller...${NC}"
"$PYTHON_EXE" -m PyInstaller hdf5_export.spec --clean

if [ $? -eq 0 ]; then
    echo -e "${GREEN}Build successful!${NC}"
    
    # Check output path by platform
    if [ "$IS_MAC" = true ]; then
        APP_PATH="dist/hdf5_export_app.app"
        if [ ! -d "$APP_PATH" ]; then
            echo -e "${RED}ERROR: Application bundle not found at $APP_PATH${NC}"
            exit 1
        fi
        echo -e "${GREEN}Application bundle: $APP_PATH${NC}"
        APP_SIZE=$(du -sh "$APP_PATH" | cut -f1)
        echo -e "${CYAN}Bundle size: $APP_SIZE${NC}"

        # Create .dmg if requested
        if [ "$CREATE_DMG" = true ]; then
            echo -e "${CYAN}Creating DMG...${NC}"
            DMG_NAME="hdf5_export_app"
            DMG_PATH="dist/${DMG_NAME}.dmg"
            [ -f "$DMG_PATH" ] && rm -f "$DMG_PATH"
            hdiutil create -volname "HDF5 Export App" -srcfolder "$APP_PATH" -ov -format UDZO "$DMG_PATH"
            if [ $? -eq 0 ]; then
                echo -e "${GREEN}DMG created: $DMG_PATH${NC}"
            else
                echo -e "${YELLOW}Warning: DMG creation failed. You can create it manually using:${NC}"
                echo -e "${CYAN}hdiutil create -volname \"HDF5 Export App\" -srcfolder \"$APP_PATH\" -ov -format UDZO \"$DMG_PATH\"${NC}"
            fi
        else
            echo -e "${CYAN}To create a DMG, run: $0 --dmg${NC}"
        fi
    elif [ "$IS_LINUX" = true ]; then
        APP_PATH="dist/hdf5_export_app"
        if [ ! -f "$APP_PATH" ]; then
            echo -e "${RED}ERROR: Executable not found at $APP_PATH${NC}"
            exit 1
        fi
        echo -e "${GREEN}Executable: $APP_PATH${NC}"
        APP_SIZE=$(du -sh "$APP_PATH" | cut -f1)
        echo -e "${CYAN}Executable size: $APP_SIZE${NC}"
        if [ "$CREATE_DMG" = true ]; then
            echo -e "${YELLOW}Warning: --dmg is only supported on macOS and was ignored.${NC}"
        fi
    else
        echo -e "${YELLOW}No platform-specific artifact checks were run for OS '$OS_NAME'.${NC}"
    fi
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}Done!${NC}"
