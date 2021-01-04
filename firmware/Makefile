SDCC=sdcc
SDLD=sdld
OBJECT=blinky.ihx

.PHONY: all clean flash

all: $(OBJECT)

clean:
	rm -f $(OBJECT)

flash: $(OBJECT)
	STVP_CmdLine.exe -Device=STM8L15xG6 -FileProg=$(OBJECT) -verif -no_loop
	# stm8flash -cstlink -pstm8l150 -w $(OBJECT).ihx

%.ihx: %.c
	$(SDCC) -lstm8 -mstm8 --out-fmt-ihx --debug $(CFLAGS) $(LDFLAGS) $<
