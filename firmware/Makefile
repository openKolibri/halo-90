SDCC=sdcc
SDLD=sdld
OBJECT=halo.ihx

.PHONY: all clean flash

all: $(OBJECT)

clean:
	rm -f $(OBJECT) *.cdb *.adb *.asm *.li *.lst *.map *.rel *.rst *.sym *.lk

flash: $(OBJECT)
	STVP_CmdLine.exe -Device=STM8L15xG4 -FileProg=$(OBJECT) -verif -no_loop -no_log
	# stm8flash -cstlink -pstm8l150 -w $(OBJECT)

%.ihx: %.c
	$(SDCC) -lstm8 -mstm8 --out-fmt-ihx --debug $(CFLAGS) $(LDFLAGS) $<
