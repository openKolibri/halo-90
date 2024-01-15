SDCC=sdcc
SDLD=sdld
OBJECT=halo.ihx

.PHONY: all clean flash

all: $(OBJECT)

clean:
	rm -f $(OBJECT) *.cdb *.adb *.asm *.li *.lst *.map *.rel *.rst *.sym *.lk

flash: $(OBJECT)
ifneq (, $(shell command -v STVP_CmdLine.exe;))
	STVP_CmdLine.exe -Device=STM8L15xG4 -FileProg=$(OBJECT) -verif -no_loop -no_log
else ifneq (, $(shell command -v stm8flash;))
	stm8flash -cstlink -p"stm8l151?2" -w $(OBJECT)
else
	@echo "STVP_CmdLine or stm8flash not found"
endif
	
	

%.ihx: %.c
	$(SDCC) -lstm8 -mstm8 --out-fmt-ihx --debug $(CFLAGS) $(LDFLAGS) $<
