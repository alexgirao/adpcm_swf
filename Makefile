
C_PROGS = adpcm_swf2raw

all: $(C_PROGS)

%.o: %.c
	gcc -g -Wall -c -o $@ $<

$(C_PROGS):
	gcc -Wall -o $@ $^ -lrt

clean:
	file * | grep ' ELF.* \(executable\|relocatable\),' | cut -d: -f1 | xargs rm -fv

# depends

adpcm_swf2raw: adpcm_swf2raw.o getopt_x.o bsd-getopt_long.o debug0.o str.o

str.o: str.h
adpcm_swf2raw.o: adpcm_swf2raw.c str.h debug0.h
