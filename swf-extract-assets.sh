#!/bin/bash
#
# relies on: swftools 0.9.2
# relies on: SoX v14.3.0
#

set -eu #x

umask 022

die(){
    echo $2
    exit $1
}

[ "$#" -gt 0 ] || die 1 "usage: $0 file1 file2 ..."

sox22khz16bitmono="sox --rate 22050 --channels 1 --bits 16 --encoding signed-integer --endian little -t raw"
sox44khz16bitmono="sox --rate 44100 --channels 1 --bits 16 --encoding signed-integer --endian little -t raw"
sox5khz16bitmono="sox --rate 5512 --channels 1 --bits 16 --encoding signed-integer --endian little -t raw"

for i in ${1+"$@"}; do
    if ! test -f "$i"; then
        echo "error: file [$i] not found"
        continue
    fi

    ext="${i##*.}"
    dirname="${i%/*}"
    basename0="${i##*/}"
    basename="${basename0%%.*}"

    if [ "x$dirname" = "x$i" ]; then
        outdir="${basename}.${ext}_assets"
    else
        outdir="${dirname}/${basename}.${ext}_assets"
    fi

    dumpfile_gz="${outdir}/swfdump.txt.gz"

    ####

    test -d "${outdir}" || mkdir -p "${outdir}"
    test -f "${dumpfile_gz}" || swfdump --full "${i}" | gzip > "${dumpfile_gz}"

    #  DEFINESOUND defines id ???? (ADPCM 22Khz 16Bit mono)

    for j in $(zcat "${dumpfile_gz}" | perl -lane 'if ($_ =~ / DEFINESOUND defines id .... \(ADPCM 22Khz 16Bit mono\)/) { print $F[5]; }'); do
	if [ ! -f "${outdir}/sound-${j}.adpcm" ]; then
	    swfextract -s "$j" -o "${outdir}/sound-${j}.adpcm" "${i}"
	fi
	if [ ! -f "${outdir}/sound-${j}.adpcm.wav" -o "${outdir}/sound-${j}.adpcm" -nt "${outdir}/sound-${j}.adpcm.wav" ]; then
	    rm -f "${outdir}/sound-${j}.adpcm.wav"
	    adpcm_swf2raw -i "${outdir}/sound-${j}.adpcm" -o - | \
		$sox22khz16bitmono - "${outdir}/sound-${j}.adpcm.wav"
	fi
    done

    #  DEFINESOUND defines id ???? (ADPCM 44Khz 16Bit mono)

    for j in $(zcat "${dumpfile_gz}" | perl -lane 'if ($_ =~ / DEFINESOUND defines id .... \(ADPCM 44Khz 16Bit mono\)/) { print $F[5]; }'); do
	if [ ! -f "${outdir}/sound-${j}.adpcm" ]; then
	    swfextract -s "$j" -o "${outdir}/sound-${j}.adpcm" "${i}"
	fi
	if [ ! -f "${outdir}/sound-${j}.adpcm.wav" -o "${outdir}/sound-${j}.adpcm" -nt "${outdir}/sound-${j}.adpcm.wav" ]; then
	    rm -f "${outdir}/sound-${j}.adpcm.wav"
	    adpcm_swf2raw -i "${outdir}/sound-${j}.adpcm" -o - | \
		$sox44khz16bitmono - "${outdir}/sound-${j}.adpcm.wav"
	fi
    done

    # DEFINESOUND defines id ???? (ADPCM 5.5Khz 16Bit mono)

    for j in $(zcat "${dumpfile_gz}" | perl -lane 'if ($_ =~ / DEFINESOUND defines id .... \(ADPCM 5\.5Khz 16Bit mono\)/) { print $F[5]; }'); do
	if [ ! -f "${outdir}/sound-${j}.adpcm" ]; then
	    swfextract -s "$j" -o "${outdir}/sound-${j}.adpcm" "${i}"
	fi
	if [ ! -f "${outdir}/sound-${j}.adpcm.wav" -o "${outdir}/sound-${j}.adpcm" -nt "${outdir}/sound-${j}.adpcm.wav" ]; then
	    rm -f "${outdir}/sound-${j}.adpcm.wav"
	    adpcm_swf2raw -i "${outdir}/sound-${j}.adpcm" -o - | \
		$sox5khz16bitmono - "${outdir}/sound-${j}.adpcm.wav"
	fi
    done

    #  DEFINESOUND defines id 0001 (MP3 22Khz 16Bit mono)

    for j in $(zcat "${dumpfile_gz}" | perl -lane 'if ($_ =~ / DEFINESOUND defines id .... \(MP3 /) { print $F[5]; }'); do
	if [ ! -f "${outdir}/sound-${j}.mp3" ]; then
	    swfextract -s "$j" -o "${outdir}/sound-${j}.mp3" "${i}"
	fi
    done

done
