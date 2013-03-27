#!/bin/bash

# Remove any old files
find . -maxdepth 1 -name \*.zip -delete
find . -maxdepth 1 -type d -exec rm -rf {}/* \;

# Generate Gerbers for each pcb file in the parent directory
for pcbname in `ls .. |sed -n -e '/\.pcb/s/\.pcb$//p'`; do
    if [[ ! -e $pcbname ]]; then
	mkdir $pcbname
    fi
    pcb -x gerber --all-layers --name-style fixed --gerberfile $pcbname/$pcbname ../$pcbname.pcb
done

# Remove Paste files, OSHPark doesn't do stencils
find . -name \*paste\* -delete

# Remove empty silk layers
find . -name \*silk\* -size -380c -delete

# Compress Gerbers
find . -maxdepth 1 -type d -and -not -name '.' -exec zip -r {} {} \;
