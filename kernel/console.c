// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "traps.h"
#include "spinlock.h"
#include "file.h"
#include "memlayout.h"
#include "proc.h"
#include "x86.h"
#include "printf.h"

#include <stdarg.h>

static void consputc(int);

static int panicked = 0;

static struct {
	struct spinlock lock;
	int locking;
} cons;

static int search_active_flag = 0; //flag da li je aktivan search bafer
static char search_buf[81];//bafer sa karakterima koje trazimo
static int search_len = 0;//duzina niza koji trazimo
static unsigned short screen_backup[80];//cuvanje sadrzaja poslednjeg reda
static int current_match = 0;// broj po redu koji gledamo od onih koji su selektovani

static int char_lower(int c) {
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	return c;
}

static void refresh_search(void) {
	int i, j, found_count = 0;
	unsigned short *crt = (unsigned short*) P2V(0xb8000); // pointer za video memoriju

	for(i = 0; i < 24 * 80; i++){
		if(search_active_flag)
			crt[i] = (crt[i] & 0xff) | 0xF000; //pozadina bela za aktivan
		else
			crt[i] = (crt[i] & 0xff) | 0x0700; // normalna crna pozadina
	}

	if(search_len == 0 || !search_active_flag) return;

	for(i = 0; i < 24 * 80; i++){
		int match = 1;

		for(j = 0;j < search_len; j++){ //petlja prolazi i gleda da li se karakteri poklapaju sa search baferom (da li je pronadjen niz slova koji treba da se selektuje bojom)
			if(i+j >= 24 * 80 || char_lower(crt[i+j] & 0xff) != char_lower(search_buf[j])){
				match = 0;
				break;
			}
		}


	if(match) {
		unsigned short color = (found_count == current_match) ? 0x2F00 : 0x5F00; // menja boju za selektovanu pronadjenu rec
			for(j = 0; j < search_len; j++)
				crt[i+j] = (crt[i+j] & 0xff) | color;
		found_count++;

		}
	}
	if (found_count > 0) { // kruzna navigacija
		if (current_match >= found_count) current_match = 0;
		if (current_match < 0) current_match = found_count - 1;
	} else {
		current_match = 0;
	}
}

static void toggle_search(void){
	int i;
	unsigned short *crt = (unsigned short*)P2V(0xb8000); //Uzima posledju liniju i cuva je da bi se kasnije vratila pri izlasku iz search moda

	if(!search_active_flag){
		search_active_flag = 1;

		for(i = 0; i< 80; i++) screen_backup[i] = crt[24*80 + i]; //pamti poslednju liniju da bi se vratila nakon search-a

		char *prompt = "Search: "; //pamti adrese Search i sta je tu ostalo ukucano
		for(i = 0; i < 8; i++) crt[24*80 + i] = (prompt[i] & 0xff) | 0x0A00; // ispisuje dole Search:

		for(i = 0; i < search_len; i++)
			crt[24*80 + 8 + i] = (search_buf[i] & 0xff) | 0x0800; //ispisuje ostatak zapamcenog teksta ako postoji u search

		refresh_search();
	} else {
		search_active_flag = 0;

		for(i = 0; i < 80; i++) crt[24*80 + i] = screen_backup[i]; // ova linija vraca backup nazad nakon searcha
		refresh_search();

	}

}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int locking;
	va_list args;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	va_start(args, fmt);

	fnvprintf(consputc, fmt, args);

	va_end(args);

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static unsigned short *crt = (unsigned short*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
	int pos;

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	if(c == '\n')
		pos += 80 - pos%80;
	else if(c == BACKSPACE){
		if(pos > 0) --pos;
	} else
		crt[pos++] = (c&0xff) | 0x0700;  // black on white

	if(pos < 0 || pos > 25*80)
		panic("pos under/overflow");

	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input;

#define C(x)  ((x)-'@')  // Control-x

void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;

	acquire(&cons.lock);
	while((c = getc()) >= 0){

		if(c == (0x1000 | 'f')){
			toggle_search();
			continue;
		}

		if(search_active_flag){
			if(c == (0x1000 | 'n')){
				current_match++;
				refresh_search();
			}else if (c == (0x1000 | 'p')){
				current_match--;
				refresh_search();
			}else if (c == BACKSPACE || c == '\x7f' || c == C('H')){ //oba su za backspace, '/x7f' je samo za sigurnost pri radjenju u svom terminalu na archu
				if(search_len > 0){
					search_len--;
					current_match = 0; // vraca korisnika na prvi pronadjeni rezultat
					crt[24*80 + 8 + search_len] = ' ' | 0x0800; //brise karakter kada se klikne backspace
					refresh_search();
				}
			}else if( c >= 32 && c <= 126 && search_len < (80 - 8)){ //samo tasteri koji su citljivi
				search_buf[search_len++] = (char)c; //dodaje slobo u search bafer i povecava duzinu trazenog niza
				crt[24*80 + 8 + (search_len - 1)] = (c & 0xff) | 0x0800; // upisuje normalan karakter
				current_match = 0;
				refresh_search();
			}
			continue;
		}

		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			break;
		case C('U'):  // Kill line.
			while(input.e != input.w &&
			      input.buf[(input.e-1) % INPUT_BUF] != '\n'){
				input.e--;
				consputc(BACKSPACE);
			}
			break;
		case C('H'): case '\x7f':  // Backspace
			if(input.e != input.w){
				input.e--;
				consputc(BACKSPACE);
			}
			break;
		default:
			if(c != 0 && input.e-input.r < INPUT_BUF){
				c = (c == '\r') ? '\n' : c;
				input.buf[input.e++ % INPUT_BUF] = c;
				consputc(c);
				if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
					input.w = input.e;
					wakeup(&input.r);
				}
			}
			break;
		}
	}
	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}

int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);
	while(n > 0){
		while(input.r == input.w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input.r, &cons.lock);
		}
		c = input.buf[input.r++ % INPUT_BUF];
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input.r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n')
			break;
	}
	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	release(&cons.lock);
	ilock(ip);

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}

