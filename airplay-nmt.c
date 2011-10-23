#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h> 
#include <signal.h>
#include <pthread.h>

#include "airplay-nmt.h"

#define PORT      6000
#define PROXYPORT 7000
#define MAX_HEADER   5120
#define MAX_RESPONSE 5120
#define MAX_SEND 5120
#define MAX_PROXY_BUFFER (64*1024)
#define NL "\r\n"

#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

#ifdef A100
static const int a100=1;
#else
static const int a100=0;
#endif

enum {
#ifdef A100
   SourceButton= 0xDD,
   UpButton=     0xA8,
   HomeButton=   0xD0,
   LeftButton=   0xAA,
   OkButton=     0x0D,
   RightButton=  0xAB,
   InfoButton=   0x95,
   DownButton=   0xA9,
   BackButton=   0x8D,
   PlayButton=   0xE9,
   PauseButton=  0xEA,
   StopButton=   0x1B,
   Number0Button=0xF1,
   TimeSeek=     0x91,
   PowerButton=  0xD2,
   EjectButton=  0xEF,
#else
   SourceButton= 'B',
   UpButton=     'U',
   HomeButton=   'O',
   LeftButton=   'L',
   OkButton=     '\n',
   RightButton=  'R',
   InfoButton=   'i',
   DownButton=   'D',
   BackButton=   'v',
   PlayButton=   'y',
   PauseButton=  'p',
   StopButton=   's',
   Number0Button='0',
   TimeSeek=     'H',
   PowerButton=  'x',
   EjectButton=  'j',
#endif
};
typedef unsigned char REMOTE_BUTTONS;

#define HTML_REPORT_ERRROR2  "<body bgcolor=black link=black>" \
                             "<font size='6' color='#ffffff'><b><br><br>%s<br>%s</b></font>" \
                             "<br><font size='6' color='#ffffff'><b>Press Return on your remote to go back to your previous location</b></font>\n"

static int last_scrub=0, last_position=0, last_duration = 0;

static struct sockaddr_in clientname, clientname_proxy; // hack: make global

enum {
   STATUS_OK=200, STATUS_SWITCHING_PROTOCOLS=101, STATUS_NOT_IMPLEMENTED=501,
};

static char airplay_url[MAX_SEND];

static int proxyon=1, proxyoff;
static int loglevel=1;

enum {LOG_PROXY, LOG_MAIN };
static char *logs[] = {"PROXY", "MAIN "};

#define WILL_LOG (!(loglevel<2))
#define log_printf(...) if (loglevel<2) {} else stamped_printf(stdout, __VA_ARGS__)
#define error_printf(...) if (loglevel<1) {} else stamped_printf(stderr, __VA_ARGS__)
#define STRLEN(s) (sizeof(s)-1)
#define perror_abort(s) do {perror(s); exit(EXIT_FAILURE);}while(0)

typedef enum {
   CLOCK_READ, CLOCK_RESET, CLOCK_PAUSE, CLOCK_RESUME
} CLOCK_MODE_T;
static unsigned get_seconds(CLOCK_MODE_T clock_mode)
{
   static time_t start;
   static time_t now;
   static int paused;
   if (clock_mode == CLOCK_RESET) start = time(NULL);
   else if (clock_mode == CLOCK_PAUSE) paused = 1;
   else if (clock_mode == CLOCK_RESUME) paused = 0;
   if (!paused) now = time(NULL);
   return now-start;
}


static int stamped_printf(FILE *fp, int log, const char *format, ...)
{
   va_list arg;
   int done;
   const time_t ltime=time(NULL); /* get current cal time */
   char buf[32];
   done = strftime(buf, sizeof buf, "%H:%M:%S", localtime(&ltime));
   done = fprintf(fp, "[%s]%s:", logs[log], buf);
   va_start(arg, format);
   done += vfprintf(fp, format, arg);
   va_end(arg);
   return done;
}

static char *get_mac_addr(void)
{
   static char mac[] = "00:00:00:00:00:00";
   char buf[256];
   FILE *fp = fopen("/sys/class/net/eth0/address", "rt");
   if (fp) {
      if (fgets(buf, sizeof buf, fp) > 0) {
         strncpy(mac, buf, sizeof(mac)-1);
         //error_printf(LOG_MAIN, "[%s][%s]\n", buf,mac);
      }
      fclose(fp);
   }
   return mac;
}

#define PROXY_PATH "http://127.0.0.1:7000/proxy?url="
#define PROXY_PATH_QT "http://127.0.0.1:7000/proxy?useragent=Quicktime&url="

static char *UrlMangle(const char *szText, char *szDst, int bufsize, int want_proxy)
{
   char ch; 
   int iMax,i,j; 

   iMax = bufsize-2;
   szDst[0]='\0';
   for (i = 0,j=0; szText[i] && j <= iMax; i++) {
      ch = szText[i];
      if (strncmp(szText+i, "http://trailers.apple.com", STRLEN("http://trailers.apple.com"))==0) {
         strcpy(szDst+j, PROXY_PATH_QT "http");
         j += STRLEN(PROXY_PATH_QT "http");
         i += STRLEN("http")-1;
      } else if (want_proxy && strncmp(szText+i, "http://", STRLEN("http://"))==0) {
         strcpy(szDst+j, PROXY_PATH "http");
         j += STRLEN(PROXY_PATH "http");
         i += STRLEN("http")-1;
      } else if (strncmp(szText+i, "://0.0.0.0", STRLEN("://0.0.0.0"))==0) {
         strcpy(szDst+j, "://");
         j += STRLEN("://");
         strcpy(szDst+j, inet_ntoa(clientname.sin_addr));
         j += strlen(inet_ntoa(clientname.sin_addr));
         i += STRLEN("://0.0.0.0")-1;
      } else {
         szDst[j++]=ch;
      }
   }
   szDst[j]='\0';
   return szDst;
}

static char *UrlEncode(const char *szText, char *szDst, int bufsize)
{
   char ch; 
   char szHex[5];
   int iMax,i,j; 

   iMax = bufsize-2;
   szDst[0]='\0';
   for (i = 0,j=0; szText[i] && j <= iMax; i++) {
      ch = szText[i];
      if (strncmp(szText+i, PROXY_PATH, STRLEN(PROXY_PATH))==0) {
         strcpy(szDst+j, PROXY_PATH);
         j += STRLEN(PROXY_PATH);
         i += STRLEN(PROXY_PATH)-1;
      } else if (strncmp(szText+i, PROXY_PATH_QT, STRLEN(PROXY_PATH_QT))==0) {
         strcpy(szDst+j, PROXY_PATH_QT);
         j += STRLEN(PROXY_PATH_QT);
         i += STRLEN(PROXY_PATH_QT)-1;
      } else if (isalnum(ch))// || ch=='/' || ch==':' || ch=='.' || ch=='_' || ch=='-' || ch=='~')
         szDst[j++]=ch;
      else if (ch == ' ')
         szDst[j++]='+';
      else {
         if (j+2 > iMax) break;
         szDst[j++]='%';
         sprintf(szHex, "%-2.2X", ch);
         strncpy(szDst+j,szHex,2);
         j += 2;
      } 
   }
   szDst[j]='\0';
   return szDst;
}

static int get_ip_addr(int log, const char *host, struct in_addr *sin_addr)
{
   struct hostent     *he;
   if ((he = gethostbyname(host)) == NULL) {
      error_printf(log, "http_request_with_response: gethostbyname(%s)=0\n", host);
      return -1;
   }
   memcpy(sin_addr, he->h_addr_list[0], sizeof sin_addr);
   return 0;
}

static int requires_proxy(int filedes, const char *url)
{
   struct in_addr sin_remote;
   int log = LOG_MAIN;
   char host[MAX_SEND];
   int s = sscanf(url, "http://%[^:]s:", host);
   log_printf(log, "requires_proxy: (%s)=%d\n", host, s);
   if (s == 1 && get_ip_addr(log, host, &sin_remote)==0 ) {
      log_printf(log, "requires_proxy: (%x)=%x\n", clientname.sin_addr.s_addr, sin_remote.s_addr);
      if ((clientname.sin_addr.s_addr >> 8) == (sin_remote.s_addr >> 8))
         return 1;
   }
   return 0;
}

static int ishex(char s)
{
   int ret;
   const char *hex = "0123456789abcdef";
   ret = strchr(hex, tolower(s)) - hex;
   return(ret >=0 && ret <16);
}

static int dehex(char s)
{
   int ret;
   const char *hex = "0123456789abcdef";
   ret = strchr(hex, tolower(s)) - hex;
   assert(ret >=0 && ret <16);
   return ret;
}

static void UrlDecode(const char *src, char *dst)
{
   const char *s = src; char *d = dst;
   while (*s) {
      if (s[0]=='%' && ishex(s[1]) && ishex(s[2]))
         *d++ = (dehex(s[1])<<4)|(dehex(s[2])<<0), s+=3;
      else *d++ = *s++;
   }
   *d++ = '\0';
}

static char *strnstr(const char *s1, const char *s2, size_t n)
{
   char safe = s1[n-1];
   ((char *)s1)[n-1] = '\0';
   char *s = strstr(s1, s2);
   ((char *)s1)[n-1] = safe;
   return s;
}

/* Wat for input on socket with timeout. */
static int wait_for_socket(int log, int sockfd, int timeout)
{
return 1;
   fd_set rfds;
   struct timeval tv;
   int retval;
   FD_ZERO(&rfds);
   FD_SET(sockfd, &rfds);

   /* Wait up to timeout microseconds. */
   tv.tv_sec = timeout / 1000000;
   tv.tv_usec = timeout % 1000000;

   retval = select(1, &rfds, NULL, NULL, &tv);
   /* Don’t rely on the value of tv now! */
   log_printf(log, "wait_for_socket(%d)=%d\n", sockfd, retval);
   return retval;
}

/* reads http header until response_size, NL NL or EOF.
   may have read some of body, which will be pointed to by body (first byte after NL NL)
   returns bytes read into response, or negative for error
*/
static int http_read_header(int log, int sockfd, char *response, int response_size, char **body, int *body_size)
{
   int bytes_read=0;
   if (!response) return 0;
   if (body) *body = NULL;
   if (body_size) *body_size = 0;
   while (1) {
      int nbytes = read(sockfd, response+bytes_read, response_size-bytes_read);
      //error_printf(log, "% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, response+bytes_read);
      //log_printf(log, "http_read_header: read=%d, %d/%d\n", nbytes, bytes_read, response_size);
      if (nbytes < 0) {
         /* Read error. */
         //perror_abort("http_read_header");
         break;
      } else if (nbytes == 0) {
         /* End-of-file. */
         break;
      } else {
         /* Data read. */
         bytes_read += nbytes;
         assert(bytes_read <= response_size);
         char *newlines = strnstr(response, NL NL, response_size);
         if (newlines) { // end of header
            newlines += strlen(NL NL);
            char *found=strnstr(response, "Content-Length:", newlines-response);
            if (body_size) *body_size = found ? atoi(found + STRLEN("Content-Length:")):0;
            if (body) *body = newlines;
            break;
         }
      }
   }
   int header_size = body && *body ? *body - response: bytes_read;
   log_printf(log, "http_read_header(%d): DONE %d/%d\n[%.*s]...%d\n", sockfd, bytes_read, response_size, header_size, response, body_size ? *body_size:0 );
   return bytes_read;
}


static int socket_read(int log, int sockfd, char *buffer, int length)
{
   int bytes_read=0, nbytes = 0;
   int remaining = length;
   assert(buffer);
   while (1) {
      nbytes = read(sockfd, buffer+bytes_read, remaining);
      //error_printf(log, "% 6d: [%.*s]\n", nbytes, nbytes > 0 ? 0:nbytes, buffer+bytes_read);
      //log_printf(log, "% 6d %d\n", nbytes, remaining);
      if (nbytes < 0) {
         /* Read error. */
         //perror_abort("socket_read");
         break;
      } else if (nbytes == 0) {
         /* End-of-file. */
         break;
      } else {
         /* Data read. */
         bytes_read += nbytes;
         remaining -= nbytes;
         assert(bytes_read <= length);
      }
   }
   if (bytes_read < 4096) log_printf(log, "socket_read(%d): DONE %d/%d (%d)\n[%.*s]\n", sockfd, bytes_read, length, remaining, bytes_read, buffer);
   else log_printf(log, "socket_read(%d): DONE %d/%d (%d)\n", sockfd, bytes_read, length, remaining);
   return bytes_read > 0 ? bytes_read : nbytes;
}

static int socket_write(int log, int sockfd, const char *buffer, int length)
{
   int bytes_written=0;
   int remaining = length;
   if (buffer) while (1) {
         int nbytes = write(sockfd, buffer+bytes_written, remaining);
         //error_printf(LOG_MAIN, "% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, buffer+bytes_written);
         //printf("socket_write: read=%d, %d/%d\n", nbytes, bytes_read, response_size);
         if (nbytes < 0) {
            /* Write error. */
            if (bytes_written < length) {
               error_printf(log, "socket_write(%d)=%d: DONE %d/%d (%d)\n", sockfd, nbytes, bytes_written, length, remaining);
               break;
               //perror_abort("socket_write");
            }
            break;
         } else if (nbytes == 0) {
            /* End-of-file. */
            break;
         } else {
            /* Data read. */
            bytes_written += nbytes;
            remaining -= nbytes;
         }
      }
   if (bytes_written < 4096) log_printf(log, "socket_write(%d): DONE %d/%d (%d)\n[%.*s]\n", sockfd, bytes_written, length, remaining, bytes_written, buffer);
   else log_printf(log, "socket_write(%d): DONE %d/%d (%d)\n", sockfd, bytes_written, length, remaining);
   return bytes_written;
}

static void http_response(int log, int sockfd, int status, const char *status_string, const char *content, const char *header)
{
   char response[MAX_RESPONSE];
   const time_t ltime=time(NULL); /* get current cal time */
   const char *date = asctime(gmtime(&ltime)); //"Fri, 17 Dec 2010 11:18:01 GMT"NL;
   if (content && content[0] == '*') content++, sprintf(response, "HTTP/1.1 %d %s"NL"Date: %s", status, status_string, date);
   else sprintf(response, "HTTP/1.1 %d %s"NL "Date: %s" "Content-Length:%d"NL, status, status_string, date, content ? strlen(content)+2:0);
   if (header) strcat(response, header);
   if (content) strcat(response, ""NL), strcat(response, content);
   strcat(response, ""NL);
   //if (status != STATUS_SWITCHING_PROTOCOLS) {log_printf(LOG_MAIN, "{%s}\n", response);}
   int s = socket_write(log, sockfd, response, strlen(response));
   assert(s==strlen(response));
   //log_printf(log, "http_response(%d): DONE (%d)\n[%.*s]\n", sockfd, s, s, content);
}


static int make_socket_out(int log, struct in_addr sin_addr, int port)
{
   struct sockaddr_in  server;
   int s, sockfd = socket(AF_INET,SOCK_STREAM,0);

   if (sockfd<0) {
      error_printf(log, "make_socket_out(%d.%d.%d.%d:%d)=%d\n", (sin_addr.s_addr>>0)&0xff,(sin_addr.s_addr>>8)&0xff,(sin_addr.s_addr>>16)&0xff,(sin_addr.s_addr>>24)&0xff, port, sockfd);
      return sockfd;
   }
   /*
    * copy the network address part of the structure to the 
    * sockaddr_in structure which is passed to connect() 
    */
   memcpy(&server.sin_addr, &sin_addr, sizeof server.sin_addr);
   server.sin_family = AF_INET;
   server.sin_port = htons(port);

   /* connect */
   if (s = connect(sockfd, (struct sockaddr *)&server, sizeof server), s) {
      error_printf(log, "connect(%d.%d.%d.%d:%d)(%d)=%d\n", (sin_addr.s_addr>>0)&0xff,(sin_addr.s_addr>>8)&0xff,(sin_addr.s_addr>>16)&0xff,(sin_addr.s_addr>>24)&0xff, port, sockfd, s);
      return -1;
   }
   log_printf(log, "make_socket_out(%d.%d.%d.%d:%d)=%d\n", (sin_addr.s_addr>>0)&0xff,(sin_addr.s_addr>>8)&0xff,(sin_addr.s_addr>>16)&0xff,(sin_addr.s_addr>>24)&0xff, port, sockfd);
   return sockfd;
}


int sendCommandGetResponse(int log, struct in_addr sin_addr, int port, const char *cmd, char *response, int response_size)
{
   int sockfd=make_socket_out(log, sin_addr, port);
   if (sockfd < 0) return sockfd;
   int s = socket_write(log, sockfd, cmd, strlen(cmd));
   assert(s == strlen(cmd));
   int bytes_read = socket_read(log, sockfd, response, response_size);
   close(sockfd);
   response[min(bytes_read, response_size-1)] = '\0';
   return 0;
}

static void ignore_signal(int sig) {
   error_printf(LOG_MAIN, "Ignored signal %d\n", sig);
}

static void ex_program(int sig) {
#if 1
   error_printf(LOG_MAIN, "Wake up call ... !!! - Caught signal: %d ... !!\n", sig);
   fflush(stdout);
   fflush(stderr);
   exit(EXIT_FAILURE);
#else
   close (sock);
   FD_CLR (sock, &active_fd_set);
   (void) signal(SIGINT, SIG_DFL);
#endif
}

static int http_request_with_response(int log, const char *url, char *response, int response_size)
{
   char command[MAX_SEND];
   char host[MAX_SEND] = "localhost";
   char *p;
   int port = 80;
   int s;
   struct in_addr sin_addr;
   s = sscanf(url, "http://%[^:]s:", host);
   p = strstr(url, "//");
   if (p) p = strstr(p, ":");
   if (p) port = atoi(p+1);
   if (p) p = strstr(p, "/");
   if (!p) return -1;
   log_printf(log, "parsed [%s] to [%s] %d (%d)\n", url, host, port, s);

   struct hostent     *he;
   if ((he = gethostbyname(host)) == NULL) {
      error_printf(log, "http_request_with_response: gethostbyname(%s)=0\n", host);
      return -1;
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);
   sprintf(command, 
           "GET %.*s HTTP/1.1"NL
           "User-Agent: Wget/1.11.2"NL
           "Accept: */*"NL
           "Host: %.*s"NL
           "Connection: Keep-Alive"NL
           , url+strlen(url)-p, p, p-url, url);
   return sendCommandGetResponse(log, sin_addr, port, command, response, response_size);
}

static int http_request_and_parse_nums(int log, const char *command, const char *key[], int *value)
{
   char response[MAX_RESPONSE];
   int s = http_request_with_response(log, command, response, sizeof response);
   char *p; int v=0;
   while (s==0 && key && *key) {
      if (*key[0] == '?') { // boolean
         p = strstr(response, *key+1);
         v = p ? 1:0;
      } else {
         p = strstr(response, *key);
         v = p ? atoi(p+strlen(*key)):0;
      }
      if (value) *value = v;
      //printf("%s=%d\n", *key, *value);
      value++, key++;
   }
   return s;
}

typedef struct BPListTrailer {
   uint8_t           unused[6];
   uint8_t           offsetIntSize;
   uint8_t           objectRefSize;
   uint64_t       objectCount;
   uint64_t       topLevelObject;
   uint64_t       offsetTableOffset;
} BPListTrailer;

typedef struct BPListState {
   unsigned char *p;
   int i; // current offset
   int body_length;
   char *content, *end;
   BPListTrailer trailer;
} BPListState;

static int ReadBObjectAt(BPListState *s, int objnum);

static unsigned bplist_readInt(const void *q, int objLen)
{
   const unsigned char *p = q;
   unsigned x = 0;
   assert(objLen >=1 && objLen <=8);
   while (objLen-->0) x = (x<<8)|*p++;
   return x;
}

static int readBType0(BPListState *s, int objLen) // simple
{
   int ans = 0;
   if (objLen == 0) ans = 0; // false
   else if (objLen == 9) ans = 1; // true
   else assert(0);
   return ans;
}

static int readBType1(BPListState *s, int objLen) // int
{
   int ans = bplist_readInt(s->p+s->i, 1<<objLen);
   s->i += 1<<objLen;
   return ans;
}

static int readBType2(BPListState *s, int objLen) // real
{
   int ans = 0;
   s->i += 1<<objLen;
   return ans;
}

static int readBType3(BPListState *s, int objLen) // date
{
   int ans = 0;
   s->i += 8;
   return ans;
}

static int readBType4(BPListState *s, int objLen) // data
{
   int ans = s->i | (objLen << 16);
   log_printf(LOG_MAIN, "bplist data [%.*s]\n", objLen, s->p+s->i);
   s->i += objLen;
   return ans;
}

static int readBType5(BPListState *s, int objLen) // ascii string
{
   int ans = s->i | (objLen << 16);
   log_printf(LOG_MAIN, "bplist string [%.*s]\n", objLen, s->p+s->i);
   s->i += objLen;
   return ans;
}

static int readBType6(BPListState *s, int objLen) // unicode string
{
   int ans = s->i | (objLen << 16);
   log_printf(LOG_MAIN, "bplist ustring [%.*s]\n", objLen, s->p+s->i);
   s->i += objLen;
   return ans;
}

static int readBTypeD(BPListState *s, int objLen) // Dictionary
{
   int ans = 0;
   int i;
   unsigned char *keys, *objs;

   log_printf(LOG_MAIN, "bplist dictionary [%.*s]\n", objLen, s->p+s->i);

   keys = s->p+s->i;
   s->i += objLen*s->trailer.objectRefSize;
   objs = s->p+s->i;
   s->i += objLen*s->trailer.objectRefSize;

   for (i=0; i<objLen; i++) {
      int key = bplist_readInt(keys + i*s->trailer.objectRefSize, s->trailer.objectRefSize);
      int obj = bplist_readInt(objs + i*s->trailer.objectRefSize, s->trailer.objectRefSize);
      unsigned int propans  = ReadBObjectAt(s, key);
      unsigned int valueans = ReadBObjectAt(s, obj);
      char *prop  = (char *)s->p + (propans & 0xffff);
      char *value = (char *)s->p + (valueans & 0xffff);
      if (strncmp(prop, "Content-Location", STRLEN("Content-Location"))==0)
         s->content = value, s->end = value + (valueans>>16);
   }
   return ans;
}

static int readBTypeX(BPListState *s, int objLen) // unimplemented
{
   error_printf(LOG_MAIN, "bplist unimplemented(%d)\n", objLen);
   return 0;
}

typedef int (*readBType_func_t)(BPListState *s, int objLen);
static readBType_func_t readBType[] = {
   readBType0, readBType1, readBType2, readBType3, readBType4, readBType5, readBType6, readBTypeX,
   readBTypeX, readBTypeX, readBTypeX, readBTypeX, readBTypeX, readBTypeD, readBTypeX, readBTypeX,
};

static int ReadBObject(BPListState *s)
{
   int ans = 0;
   log_printf(LOG_MAIN, "ReadBObject: %i\t %02X\n", s->i, s->p[s->i]);

   int objType = s->p[s->i++], objLen = objType & 0xf;
   objType >>= 4;
   if (objType != 0 && objLen == 0xf)
      objLen = ReadBObject(s);

   return readBType[objType](s, objLen);
}


static int ReadBObjectAt(BPListState *s, int objNum)
{
   log_printf(LOG_MAIN, "ReadBObjectAt(%d): %i\t %02X\n", objNum, s->i, s->p[s->i]);
   s->i = bplist_readInt(s->p + s->trailer.offsetTableOffset + objNum * s->trailer.offsetIntSize, s->trailer.offsetIntSize);
   return ReadBObject(s);
}


static char *bplist_find_content(char *body, int body_length, char **end)
{
   char *content = NULL;
   int i=0, size;
   BPListState static_s, *s=&static_s;
   char *debug_out;
   assert(strncmp("bplist00", body, 8)==0);
   memset(s, 0, sizeof *s);
   s->p = (unsigned char *)body;
   s->i = 8;
   s->body_length = body_length;
   BPListTrailer *trailer = (BPListTrailer *)(body + body_length - sizeof *trailer);
   s->trailer = *trailer;
   if (WILL_LOG) {
      debug_out = malloc(3*body_length+1);
      for (i=0; i<body_length; i++) {
         sprintf(debug_out+i, "%c", s->p[i]?s->p[i]:' ');
      }
   }
   s->trailer.objectCount = bplist_readInt(&trailer->objectCount, 1<<3);
   s->trailer.topLevelObject = bplist_readInt(&trailer->topLevelObject, 1<<3);
   s->trailer.offsetTableOffset= bplist_readInt(&trailer->offsetTableOffset, 1<<3);
   log_printf(LOG_MAIN, "bplist_parse_content: offsetIntSize=%d, objectRefSize=%d, objectCount=%d, topLevelObject=%d, offsetTableOffset=%d\n[%.*s]\n", 
              (int)s->trailer.offsetIntSize, (int)s->trailer.objectRefSize, (int)s->trailer.objectCount, (int)s->trailer.topLevelObject, (int)s->trailer.offsetTableOffset, body_length, debug_out);

   if (WILL_LOG) {
      for (i=0; i<body_length; i++) {
         sprintf(debug_out+i*3, "%02X ", s->p[i]);
      }
      log_printf(LOG_MAIN, "bplist_parse_content: %s\n", debug_out);
      free(debug_out);
   }

   ReadBObjectAt(s, s->trailer.topLevelObject);
   *end = s->end;
   return s->content;
}


#if 0
static int http_request_and_parse_num(const char *command, const char *key, int *value)
{
   const char *keys[2] = {key, NULL};
   return http_request_and_parse_nums(command, keys, value);
}
#endif

static int http_request(int log, const char *command)
{
   char response[MAX_RESPONSE];
   int s = http_request_with_response(log, command, response, sizeof response);
   return s;
}

typedef enum {
   MEDIA_STOP, MEDIA_PLAY, MEDIA_PAUSE, MEDIA_RESUME, MEDIA_SEEK, MEDIA_PHOTO
} MEDIA_MODES_T;
static const char *modename[] = {"MEDIA_STOP", "MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_RESUME", "MEDIA_SEEK", "MEDIA_PHOTO"};
#define countof(x) (sizeof x/sizeof *(x))

#ifdef A100
static int get_media_info(int *position, int *duration, int *playing, int *paused, int *stopped, int *buffering, int *seekable)
{
   if (position) *position =0;
   if (duration) *duration =0;
   if (playing)  *playing  =1;
   if (paused)   *paused   =0;
   if (stopped)  *stopped  =0;
   if (buffering)*buffering=0;
   if (seekable) *seekable =0;
   if (position) *position = last_position + get_seconds(CLOCK_READ);
   return 0;
}
#else
static int get_media_info(int *position, int *duration, int *playing, int *paused, int *stopped, int *buffering, int *seekable)
{
   const char *keys[] = {"?<returnValue>0", "<currentTime>", "<totalTime>", "?<currentStatus>play", "?<currentStatus>pause", "?<currentStatus>stop", "?<currentStatus>buffering", "?<seekEnable>true", NULL};
   int values[countof(keys)]={0};
   http_request_and_parse_nums(LOG_MAIN, "http://127.0.0.1:8008/playback?arg0=get_current_vod_info", keys, values);
   if (position) *position =values[1];
   if (duration) *duration =values[2];
   if (playing)  *playing  =values[3];
   if (paused)   *paused   =values[4];
   if (stopped)  *stopped  =values[5];
   if (buffering)*buffering=values[6];
   if (seekable) *seekable =values[7];
   return !values[0];
}
static int get_photo_info(int *position, int *duration, int *playing, int *paused, int *stopped, int *buffering, int *seekable)
{
   const char *keys[] = {"?<returnValue>0", "<currentTime>", "<totalTime>", "?<currentStatus>play", "?<currentStatus>pause", "?<currentStatus>stop", "?<currentStatus>buffering", "?<seekEnable>true", NULL};
   int values[countof(keys)]={0};
   http_request_and_parse_nums(LOG_MAIN, "http://127.0.0.1:8008/playback?arg0=get_current_pod_info", keys, values);
   if (position) *position =values[1];
   if (duration) *duration =values[2];
   if (playing)  *playing  =values[3];
   if (paused)   *paused   =values[4];
   if (stopped)  *stopped  =values[5];
   if (buffering)*buffering=values[6];
   if (seekable) *seekable =values[7];
   return !values[0];
}

static int get_system_mode(int *browser, int *pod_playback, int *vod_playback)
{
   const char *keys[] = {"?<returnValue>0", "?<apps>browser", "?<apps>POD_playback", "<apps>VOD_playback", NULL};
   int values[countof(keys)]={0};
   http_request_and_parse_nums(LOG_MAIN, "http://127.0.0.1:8008/system?arg0=get_current_app", keys, values);
   if (browser) *browser=values[1];
   if (pod_playback) *pod_playback =values[2];
   if (vod_playback) *vod_playback=values[3];
   return !values[0];
}
#endif
#ifdef A100
static void send_ir_keys(REMOTE_BUTTONS *code, int num)
{
   FILE *fp = fopen("/tmp/irkey", "wb");
   if (fp) {
      int i;
      for (i=0; i<num; i++) {
         fprintf (fp ,"%d\n", code[i]);
         log_printf(LOG_MAIN, "Wrote %d to /tmp/ir_key\n", code[i]);
      }
      fclose(fp);
   } else assert(0);
}
#else
static void send_ir_keys(REMOTE_BUTTONS *code, int num)
{
   int s, sockfd;
   struct in_addr sin_addr;
   struct hostent     *he;
   int log=LOG_MAIN;
   char *host="localhost";
   if ((he = gethostbyname(host)) == NULL) {
      error_printf(LOG_MAIN, "send_ir_keys: gethostbyname(%s)=0\n", host);
      return;
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);

   sockfd = make_socket_out(LOG_MAIN, sin_addr, 30000);
   if (sockfd < 0) {
      error_printf(log, "failed to send_ir_keys\n");
      return;
   }
   s = socket_write(log, sockfd, (char *)code, num);
   if (s != num) {
      error_printf(log, "send_ir_keys: send(%d) %d!=%d\n", sockfd, s, num);
   }
   close(sockfd);
}
#endif
static void send_ir_key(REMOTE_BUTTONS code)
{
   send_ir_keys(&code, 1);
}
#ifndef A100
static int wait_media_ready(MEDIA_MODES_T mode, MEDIA_MODES_T last_mode, int seek_offset, int *position, int *duration)
{
   int seekable = 0, playing = 0, paused=0, stopped=0, buffering=0;
   int timeout = 0;
   int media_status;
   assert(position && duration);
   while (1) {
      if (mode == MEDIA_PHOTO || last_mode == MEDIA_PHOTO) {
         media_status = get_photo_info(position, duration, &playing, &paused, &stopped, &buffering, &seekable);
         if (media_status!=0) {
            log_printf(LOG_MAIN, "************ media_status = %d ****************\n", media_status);
            stopped = 1;
         }
      } else {
         media_status = get_media_info(position, duration, &playing, &paused, &stopped, &buffering, &seekable);
         if (media_status!=0) {
            log_printf(LOG_MAIN, "************ media_status = %d ****************\n", media_status);
            //return media_status;
         }
      }
      if (mode == MEDIA_PAUSE) {
         if (paused == 1 && !buffering && seekable)
            break;
      } else if (mode == MEDIA_RESUME) {
         if (paused == 0 && !buffering && seekable)
            break;
      } else if (mode == MEDIA_STOP) {
         if (stopped && !buffering)
            break;
      } else if (mode == MEDIA_SEEK) {
         if (seek_offset < 0)
            break;
         if (seekable && !buffering)
            break;
      } else if (mode == MEDIA_PLAY) {
         if (playing && !buffering && seekable && !paused)
            break;
         if (0 && !buffering && paused && timeout>=900) {
            error_printf(LOG_MAIN, "Stuck in paused - resuming: wait_media_ready(%s->%s, %d, %d, %d) failed (%d,%d,%d,%d,%d,%d,%d)\n", 
                         modename[last_mode], modename[mode], seek_offset, *position, *duration,
                         seekable, playing, paused, stopped, buffering, 0, media_status );
            // for no good reason youtube videos end up in paused state here
            int s = http_request(LOG_MAIN, "http://127.0.0.1:8008/playback?arg0=resume_vod");
            assert(s==0);
         }
      } else if (mode == MEDIA_PHOTO) {
         if (playing)
            break;
      }
      if (timeout++ == 250) {
         log_printf(LOG_MAIN, "TIMEOUT: wait_media_ready(%s->%s, %d, %d, %d) failed (%d,%d,%d,%d,%d,%d,%d)\n", 
                    modename[last_mode], modename[mode], seek_offset, *position, *duration,
                    seekable, playing, paused, stopped, buffering, 0, media_status );
         break;
      }
      usleep(10000); // 0.1s
   }
   log_printf(LOG_MAIN, "wait_media_ready(%s->%s, %d, %d, %d) okay (%d,%d,%d,%d,%d,%d,%d)\n", 
              modename[last_mode], modename[mode], seek_offset, *position, *duration,
              seekable, playing, paused, stopped, buffering, 0, media_status );
   return media_status;
}
#endif

#ifdef A100
static void set_bookmark(const char *url, int time)
{
   char buf[MAX_SEND];
   FILE *fpin = fopen("/tmp/mono_bookmark", "rt");
   FILE *fpout = fopen("/tmp/tmp_bookmark", "wt");
   int found=0;
   if (fpin && fpout) while (!feof(fpin)) {
         if (fgets(buf, sizeof buf, fpin) == 0) break;
         if (strstr(buf, url)) {
            fprintf(fpout, "bookmark_time=%d toadj=0 bookmark_filename=%s\n", time, url);
            found = 1;
         } else {
            fprintf(fpout, "%s", buf);
         }
      }
   if (fpout && !found)
      fprintf(fpout, "bookmark_time=%d toadj=0 bookmark_filename=%s\n", time, url);
   if (fpin) fclose(fpin);
   if (fpout) fclose(fpout);
   system("cp /tmp/tmp_bookmark /tmp/mono_bookmark");
}
#endif

static int load_html(const char *format, ...)
{
   FILE *fp = fopen("/tmp/runme.html", "wb");
   if (fp) {
      va_list arg;
      va_start(arg, format);
      vfprintf(fp, format, arg);
      va_end(arg);
      fclose(fp);
   }
   if (fp) {
      fp = fopen("/tmp/gaya_bc", "wb");
   }
   if (fp) {
      fprintf (fp, "/tmp/runme.html");
      fclose(fp);
      log_printf(LOG_MAIN, "Wrote to /tmp/gaya_bc\n");
   }
   if (!fp) {
      log_printf(LOG_MAIN, "failed to open /tmp/runme.html\n");
   }
}

#ifdef A100
static int set_media_mode_ex(MEDIA_MODES_T mode, const char *url, int seek_offset, int *position, int *duration)
{
   int s, media_status=0;
   FILE *fp;
   char cmd[MAX_SEND];
   static MEDIA_MODES_T last_mode;
   int dummy_position=0, dummy_duration=0;

   if (!position) position=&dummy_position;
   if (!duration) duration=&dummy_duration;

   log_printf(LOG_MAIN, "### set_media_mode_ex(%s,%d)\n", modename[mode], seek_offset);
   if (last_mode == MEDIA_STOP && mode != MEDIA_STOP && mode != MEDIA_PLAY && mode != MEDIA_PHOTO && airplay_url[0])
      set_media_mode_ex(MEDIA_PLAY, airplay_url, seek_offset, position, duration);
   switch (mode) {
   default: break;
   case MEDIA_STOP:
      if (last_mode == MEDIA_STOP) break;
      if (last_mode != MEDIA_PHOTO) {
         send_ir_key(StopButton); usleep(3000000);
      }
      send_ir_key(BackButton);
      break;
   case MEDIA_PAUSE:
      send_ir_key(PauseButton);
      get_seconds(CLOCK_PAUSE);
      break;
   case MEDIA_RESUME:
      send_ir_key(PlayButton);
      get_seconds(CLOCK_RESUME);
      break;
   case MEDIA_SEEK:
      {
         const int hours = seek_offset / (60*60);
         const int minutes = (seek_offset % (60*60)) / 60;
         const int seconds = (seek_offset % (60*60)) % 60;
         REMOTE_BUTTONS keys[11], *k=keys;
         log_printf(LOG_MAIN, "seek_offset=%d (%02d:%02d:%02d)\n", seek_offset, hours, minutes, seconds);
         if (seek_offset <= 30) {
            send_ir_key(Number0Button);
            break;
         }
         *k++ = TimeSeek;
#ifdef A100
         *k++ = Number0Button+((hours/10)%10);
         *k++ = Number0Button+((hours)%10);
         *k++ = Number0Button+((minutes/10)%10);
         *k++ = Number0Button+((minutes)%10);
         *k++ = Number0Button+((seconds/10)%10);
         *k++ = Number0Button+((seconds)%10);
#else
         *k++ = LeftButton;
         *k++ = Number0Button+((hours/10)%10);
         *k++ = Number0Button+((hours)%10);
         *k++ = RightButton;
         *k++ = Number0Button+((minutes/10)%10);
         *k++ = Number0Button+((minutes)%10);
         *k++ = RightButton;
         *k++ = Number0Button+((seconds/10)%10);
         *k++ = Number0Button+((seconds)%10);
         *k++ = OkButton;
#endif
         send_ir_keys(keys, k-keys);
         break;
      }
   case MEDIA_PLAY:
   case MEDIA_PHOTO:
      get_seconds(CLOCK_RESET);
      UrlEncode(url, cmd, MAX_SEND-1);
      log_printf(LOG_MAIN, "Play URL %s\n", cmd);
      if (mode==MEDIA_PHOTO) {
         load_html(          "<body bgcolor=black link=black>"
                             "<center><img src=\"%s\" height=\"%d\"></center>"
                             "<a href='http://127.0.0.1:8883/start.cgi?list' tvid='home'></a>"
                             "<a href='http://127.0.0.1:8883/start.cgi?list' tvid='source'></a>", cmd, 680);
         //"<br><font size='6' color='#ffffff'><b>Press Return on your remote to go back to your previous location</b></font>\n"
      } else {
         load_html(          "<body bgcolor=black link=black onloadset='go'>"
                             "<a onfocusload name='go' href='%s' %s></a>"
                             "<a href='http://127.0.0.1:8883/start.cgi?list' tvid='home'></a>"
                             "<a href='http://127.0.0.1:8883/start.cgi?list' tvid='source'></a>"
                             "<br><font size='6' color='#ffffff'><b>Press Return on your remote to go back to your previous location</b></font>\n", cmd, mode==MEDIA_PLAY?"vod":"");
      }
      break;
   }
   last_mode = media_status==0 ? mode : MEDIA_STOP;
   return 0;
}
#else
static int set_media_mode_ex(MEDIA_MODES_T mode, const char *url, int seek_offset, int *position, int *duration)
{
   char cmd[MAX_SEND];
   int s, media_status;
   static MEDIA_MODES_T last_mode;
   int send_command = mode != last_mode;
   int dummy_position=0, dummy_duration=0;

   if (!position) position=&dummy_position;
   if (!duration) duration=&dummy_duration;
   assert(position && duration);
   log_printf(LOG_MAIN, "### set_media_mode_ex(%s,%d)\n", modename[mode], seek_offset);
   if (last_mode == MEDIA_STOP && mode != MEDIA_STOP && mode != MEDIA_PLAY && mode != MEDIA_PHOTO && airplay_url[0])
      set_media_mode_ex(MEDIA_PLAY, airplay_url, seek_offset, position, duration);
   switch (mode) {
   case MEDIA_STOP:
      if (last_mode == MEDIA_PHOTO)
         sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=stop_pod");
      else
         sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=stop_vod");
      break;
   case MEDIA_PLAY:
      strcpy(cmd, "http://127.0.0.1:8008/playback?arg0=start_vod&arg1=AirPlayVideo&arg2=");
      UrlEncode(url, cmd+strlen(cmd), MAX_SEND - STRLEN("http://127.0.0.1:8008/playback?arg0=start_vod&arg1=AirPlayVideo&arg2=") - STRLEN("&arg3=show&arg4=0")-1);
      strcat(cmd, "&arg3=show&arg4=0");
      //sprintf(cmd, "http://127.0.0.1:8080/xbmcCmds/xbmcHttp?command=PlayFile(%s)", url);
      break;
   case MEDIA_PHOTO:
      send_command = 1;
      strcpy(cmd, "http://127.0.0.1:8008/playback?arg0=start_pod&arg1=AirPlayPhoto&arg2=");
      UrlEncode(url, cmd+strlen(cmd), MAX_SEND - STRLEN("http://127.0.0.1:8008/playback?arg0=start_pod&arg1=AirPlayPhoto&arg2=") - STRLEN("&arg3=1&arg4=rot0&arg5=bghide")-1);
      strcat(cmd, "&arg3=1&arg4=rot0&arg5=bghide");
      break;
   case MEDIA_SEEK:
      assert(seek_offset >= 0);
      send_command = 1;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=set_time_seek_vod&arg1=%02d:%02d:%02d", seek_offset / 3600, (seek_offset / 60)%60, seek_offset % 60);
      break;
   case MEDIA_PAUSE:
      send_ir_key(PauseButton); send_command=0; break;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=pause_vod");
      break;
   case MEDIA_RESUME:
      if (last_mode==MEDIA_RESUME) return 0;
      send_ir_key(PlayButton); send_command=0; break;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=resume_vod");
      break;
   }
   //if (last_mode == MEDIA_STOP && mode != MEDIA_PLAY) send_command = 0;
   if (send_command) {
      s = http_request(LOG_MAIN, cmd);
      assert(s==0);
   }
   media_status = wait_media_ready(mode, last_mode, seek_offset, position, duration);
   if (mode==MEDIA_PHOTO) {
      int browser, pod_playback, vod_playback;
      s = get_system_mode(&browser, &pod_playback, &vod_playback);
      assert(s==0);
      if (!pod_playback) {
         usleep(100000);
         send_ir_key(SourceButton); send_ir_key(OkButton);
         //http_request("http://127.0.0.1:8008/system?arg0=send_key&arg2=source&arg3=browser");
         //usleep(100000);
         //http_request("http://127.0.0.1:8008/system?arg0=send_key&arg2=enter");
      }
   } else if (mode==MEDIA_STOP && last_mode==MEDIA_PHOTO) {
      int browser, pod_playback, vod_playback;
      s = get_system_mode(&browser, &pod_playback, &vod_playback);
      assert(s==0);
      if (!browser) {
         usleep(100000);
         send_ir_key(PowerButton); send_ir_key(PowerButton);
      }
   }
   if (mode==MEDIA_STOP) {
      last_scrub =-1; last_position = 0, last_duration = 0;
   }
   last_mode = media_status==0 ? mode : MEDIA_STOP;
   return media_status;
}
#endif

static int set_media_mode_url(MEDIA_MODES_T mode, const char *url)
{
   return set_media_mode_ex(mode, url, -1, NULL, NULL);
}

static int set_media_mode(MEDIA_MODES_T mode)
{
   return set_media_mode_ex(mode, NULL, -1, NULL, NULL);
}


static int make_socket_in(int log, uint16_t port)
{
   struct sockaddr_in name;
   int s, reuse_addr = 1;
   struct linger {
       int l_onoff;    /* linger active */
       int l_linger;   /* how many seconds to linger for */
   };
   struct timeval tv;
   tv.tv_sec = 0;
   tv.tv_usec = 1;

   struct linger linger;
   linger.l_onoff = 1;
   linger.l_linger = 1;

   /* Create the socket. */
   int sock = socket(PF_INET, SOCK_STREAM, 0);
   if (sock < 0) {
      error_printf(log, "make_socket_in: socket(%d)=%d\n", port, sock);
      return sock;
   }
   //setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
   //setsockopt(sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
   //setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   //setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   /* Give the socket a name. */
   memset((char *) &name, 0, sizeof name);
   name.sin_family = AF_INET;
   name.sin_port = htons(port);
   name.sin_addr.s_addr = htonl (INADDR_ANY);
   if (s = bind(sock, (struct sockaddr *) &name, sizeof name), s < 0) {
      error_printf(log, "make_socket_in: bind(%d)=%d\n", sock, s);
      return s;
   }
   if (s = listen(sock, 1), s < 0) {
      error_printf(log, "make_socket_in: listen(%d)=%d\n", sock, s);
      return s;
   }
   log_printf(log, "make_socket_in(:%d)=%d\n", port, sock);
   return sock;
}

static int media_supported(const char *url)
{
   if (strstr(url, "https://")) return 0;
   if (strstr(url, ".m3u8")) return 0;
   return 1;
}

static const char *http_status_string(int status)
{
   if (status == STATUS_OK) return "OK";
   else if (status == STATUS_NOT_IMPLEMENTED) return "Not Implemented";
   else if (status == STATUS_SWITCHING_PROTOCOLS) return "Switching Protocols";
   assert(0);
   return "Unknown";
}

static int read_from_client(int filedes)
{
   char *buffer = malloc(MAX_HEADER+1); // allow null on end
   char *content = malloc(MAX_RESPONSE);
   char *header = malloc(MAX_RESPONSE);
   int nbytes;
   char *found;
   int status = STATUS_OK;
   char *body=0; int body_size = 0;
   assert(buffer && content && header);
   content[0] = '\0';
   header[0] = '\0';

   nbytes = http_read_header(LOG_MAIN, filedes, buffer, MAX_HEADER, &body, &body_size);
   if (nbytes > 0) {
      buffer[nbytes] = '\0';
   } else {
      /* End-of-file. */
      assert(nbytes == 0);
      status = 1;
      goto fail;
   }
   if (found = strstr(buffer,"/reverse"), found) {
      status = STATUS_SWITCHING_PROTOCOLS;
      sprintf(content, "*Upgrade: PTTH/1.0"NL"Connection: Upgrade"NL);
   } else if (found = strstr(buffer,"/rate"), found) {
      found = strstr(found, "?value=");
      int rate = found ? (int)(atof(found+STRLEN("?value="))+0.5f):0;
      fprintf (stderr, "Found rate call, rate=%d\n", rate);
      set_media_mode(rate ? MEDIA_RESUME:MEDIA_PAUSE);
      if (rate && last_scrub >= 0) {
         set_media_mode_ex(MEDIA_SEEK, NULL, last_scrub, NULL, NULL);
         last_position = last_scrub;
         last_scrub = -1;
      }
   } else if (found = strstr(buffer,"/play "), found) {
      int binaryplist = strstr(buffer, "application/x-apple-binary-plist") != 0;
      if (!binaryplist) {
         found = strstr(buffer, "Content-Location: ");
         if (found) {
            found += STRLEN("Content-Location: ");
            char *newline = strchr(found, '\n');
            if (newline) *newline = '\0';
            else found = 0;
         }
      } else {
         int length=0;
         char *end = NULL;
         found = bplist_find_content(body, body_size, &end);
         if (found) {
            if (end) *end = '\0';
            else found = 0;
         }
      }
      if (found && !media_supported(found)) {
         load_html( HTML_REPORT_ERRROR2, "Media type not supported:", found);
         found = 0;
      }
      if (found) {
         int proxy= requires_proxy(filedes, found);
         fprintf (stderr, "Found content: %s (%d)\n", found, proxy);
         UrlMangle(found, airplay_url, sizeof airplay_url, (proxy | proxyon) & ~proxyoff);
#ifdef A100
         set_bookmark(airplay_url, last_scrub);
         set_media_mode_url(MEDIA_PLAY, airplay_url);
         last_position = last_scrub;
         last_scrub = -1;

#else
         if (last_scrub >= 0) {
            set_media_mode_ex(MEDIA_SEEK, NULL, last_scrub, NULL, NULL);
            last_position = last_scrub;
            last_scrub = -1;
         }
#endif
      } else {
         fprintf (stderr, "Content not found: [%s]\n", buffer);
      }
      if (!found) status = STATUS_NOT_IMPLEMENTED;
   } else if (found = strstr(buffer,"/scrub"), found) {
      int position = 0, seek_offset = -1, duration = 0;
      int itunes = 0;
      found = strstr(buffer, "User-Agent: ");
      if (found) {
         found += STRLEN("User-Agent: ");
         if (strncmp(found, "iTunes", STRLEN("iTunes"))==0)
            itunes=1;
      }
      found = strstr(buffer, "?position=");
      if (found) seek_offset = (int)(atof(found+STRLEN("?position="))+0.0f);
      if (seek_offset >= 0) {
         fprintf (stderr, "Found scrub call, position=%d\n", seek_offset);
      }
      if (seek_offset >= 0) {
         last_scrub = last_position = position = seek_offset;
         duration = 0;//last_duration;
      } else {
         int playing=0, paused=0, stopped=0, buffering=0, seekable=0;
         int media_status = get_media_info(&position, &duration, &playing, &paused, &stopped, &buffering, &seekable);
         if (media_status != 0 || !playing || paused) {
            position = last_position;
            duration = last_duration;
         } else {
#ifndef A100
            last_duration = duration;
            last_position = position;
#endif
         }
      }
      if (itunes && last_scrub >= 0) {
         set_media_mode_ex(MEDIA_SEEK, NULL, last_scrub, NULL, NULL);
         last_position = last_scrub;
         last_scrub = -1;
      }
      if (duration) sprintf(content, "duration: %f"NL"position: %f", (double)duration, (double)position);
   } else if (found = strstr(buffer,"/stop"), found) {
      fprintf (stderr, "Stop request\n");
      set_media_mode(MEDIA_STOP);
   } else if (found = strstr(buffer,"/photo"), found) {
      fprintf (stderr, "Found photo call retrieve content address\n");
      int s;
      if (body_size && body) {
         char *photo = malloc(body_size);
         const int header_size = body-buffer;
         const int already_got = nbytes-header_size;
         assert(already_got >= 0);
         assert(photo);
         memcpy(photo, body, already_got);
         s = socket_read(LOG_MAIN, filedes, photo+already_got, body_size-already_got);
         assert(s==body_size-already_got);
         FILE *fp = fopen("/tmp/airplay_photo.jpg", "wb");
         assert(fp);
         s = fwrite(photo, 1, body_size, fp);
         assert(s==body_size);
         fclose(fp);
         free(photo);
         set_media_mode_url(MEDIA_PHOTO, "file:///tmp/airplay_photo.jpg");
      }
   } else if (found = strstr(buffer,"/volume"), found) {
      // ignore
   } else if (found = strstr(buffer,"/server-info"), found) {
      sprintf(content, SERVER_INFO, get_mac_addr());
      sprintf(header, "Content-Type: text/x-apple-plist+xml"NL);
   } else if (found = strstr(buffer,"/playback-info"), found) {
      int position=0, duration=0;
      int playing=0, paused=0, stopped=0, buffering=0, seekable=0;
      int media_status = get_media_info(&position, &duration, &playing, &paused, &stopped, &buffering, &seekable);
      if (media_status != 0 || !playing || paused) {
         position = last_position;
         duration = last_duration;
      } else {
#ifndef A100
         last_duration = duration;
         last_position = position;
#endif
      }
      sprintf(content, PLAYBACK_INFO, (float)duration, (float)duration, (float)position, playing, (float)duration);
      sprintf(header, "Content-Type: text/x-apple-plist+xml"NL);
   } else if (found = strstr(buffer,"/slideshow-features"), found) {
      status = STATUS_NOT_IMPLEMENTED;
   } else if (found = strstr(buffer,"/authorize"), found) {
      load_html( HTML_REPORT_ERRROR2, "DRM protected content not supported", "");
      status = STATUS_NOT_IMPLEMENTED;
   } else if (found = strstr(buffer,"/setProperty"), found) {
      // just silently ignore for now
   } else if (found = strstr(buffer,"/getProperty"), found) {
      // just silently ignore for now
   } else {
      fprintf (stderr, "Unhandled [%s]\n", buffer);
      status = STATUS_NOT_IMPLEMENTED;
   }
   if (status) http_response(LOG_MAIN, filedes, status, http_status_string(status), content[0] ? content:NULL, header[0] ? header:NULL);
   fail:
   if (buffer) free(buffer);
   if (content) free(content);
   if (header) free(header);
   return status==STATUS_OK || status==STATUS_SWITCHING_PROTOCOLS ? 0:-status;
}

static struct proxy_s {
   struct in_addr sin_addr;
   int port;
   char host[256];
   char useragent[256];
   char httpversion;
} proxy;


/* 
   Should be able to use proxy to play Apple trailers. E.g.
   http://192.168.4.12:8008/playback?arg0=start_vod&arg1=AirPlayVideo&arg2=http://127.0.0.1:7000/proxy%3Furl=http://trailers.apple.com/movies/wb/redridinghood/redridinghood-tlr1_h480p.mov%26useragent=Quicktime&arg3=show */

/* 
GET http://127.0.0.1:7000/proxy?url=http://code.google.com/p/airplay-nmt/downloads/detail?name=airplay-nmt-r4.zip HTTP/1.0

[GET http://code.google.com/p/airplay-nmt/downloads/detail?name=airplay-nmt-r4.zip HTTP/1.0
User-Agent: Wget/1.12 (linux-gnu)
Accept: * / *
Host: code.google.com

]
*/

static int read_from_proxy(int filedes)
{
   char *buffer = malloc(MAX_PROXY_BUFFER+1); // allow null on end
   char *response_out = malloc(MAX_PROXY_BUFFER);
   int nbytes;
   char *found, *p;
   int status = -1;
   char *body=0; int body_size = 0;
   char *get = NULL;
   int s, sockfd = -1;
   assert(buffer);
   memset(&proxy, 0, sizeof proxy);

   nbytes = http_read_header(LOG_PROXY, filedes, buffer, MAX_PROXY_BUFFER, &body, &body_size);
   if (nbytes > 0) {
      buffer[nbytes] = '\0';
      assert(body_size == 0);
   } else {
      /* End-of-file. */
      assert(nbytes == 0);
      log_printf(LOG_PROXY, "%d:http_read_header: %d\n", filedes, nbytes);
      goto fail;
   }
   char *tok = buffer, *last_tok = buffer, *next_tok = NULL;
   char *d = response_out;
   if (next_tok = strstr(last_tok, NL), next_tok) tok = last_tok, *next_tok = '\0', last_tok = next_tok + strlen(NL);
   else tok = NULL;
   while (tok) {
      if (strncasecmp(tok, "GET ", STRLEN("GET ")) == 0 || strncasecmp(tok, "HEAD ", STRLEN("HEAD ")) == 0 ) {
         char *url;
         UrlDecode(tok, tok);
         if (get) {
            error_printf(LOG_PROXY, "http_read_header: got second HEAD [%s]\n", tok);
            break;
         }
         if (strncasecmp(tok, "HEAD ", STRLEN("HEAD ")) == 0 )
            get="HEAD";
         else if (strncasecmp(tok, "GET ", STRLEN("GET ")) == 0 )
            get="GET";
         if (found = strstr(tok,"/proxy?"), found) {
            found += STRLEN("/proxy?");
            if (p = strstr(found, " HTTP/1."), p) {
               proxy.httpversion = p[STRLEN(" HTTP/1.")]; *p='\0';
            }
            char *found_url = strstr(found, "url="), *found_useragent = strstr(found, "useragent=");
            if (found_url) {
               found = found_url + STRLEN("url=");
               proxy.port = 80;
               if (p=strstr(found, "://"), p) found = p + STRLEN("://");
               if (p=strstr(found, "/"), p) {
                  url = p;
               }
               if (p=strstr(found, "&useragent"), p) {
                  *p='\0';
               }
               if (p=strstr(found, "\r"), p) {
                  *p='\0';
               }
               if (p=strstr(found, "\n"), p) {
                  *p='\0';
               }
               if (p=strstr(found, ":"), p) {
                  *p='\0'; proxy.port = atoi(p+1);
               }
               if (url && found && url-found>0) strncpy(proxy.host, found, min(url-found, sizeof proxy.host));proxy.host[(sizeof proxy.host)-1] = '\0';
               struct hostent *he;
               //printf("parsed [%s] to [%s]:%d (%d.%d.%d.%d)\n", buffer, found, proxy.port, (proxy.sin_addr.s_addr>>0)&0xff,(proxy.sin_addr.s_addr>>8)&0xff,(proxy.sin_addr.s_addr>>16)&0xff,(proxy.sin_addr.s_addr>>24)&0xff);
               if ((he = gethostbyname(proxy.host)) == NULL) {
                  error_printf(LOG_PROXY, "read_from_proxy: gethostbyname(%s)=0\n", proxy.host);
                  goto fail;
               }
               memcpy(&proxy.sin_addr, he->h_addr_list[0], sizeof proxy.sin_addr);
               log_printf(LOG_PROXY, "parsed [%s] (%d.%d.%d.%d:%d)\n", proxy.host, (proxy.sin_addr.s_addr>>0)&0xff,(proxy.sin_addr.s_addr>>8)&0xff,(proxy.sin_addr.s_addr>>16)&0xff,(proxy.sin_addr.s_addr>>24)&0xff, proxy.port);
            }
            if (found_useragent) {
               found = found_useragent + STRLEN("useragent=");
               if (p=strstr(found, "&url"), p) {
                  *p='\0';
               }
               if (p=strstr(found, "\r"), p) {
                  *p='\0';
               }
               if (p=strstr(found, "\n"), p) {
                  *p='\0';
               }
               strncpy(proxy.useragent, found, sizeof proxy.useragent);
               proxy.useragent[(sizeof proxy.useragent)-1] = '\0';
               log_printf(LOG_PROXY, "parsed useragent [%s]\n", proxy.useragent);
            }
         } else {
            if (strncasecmp(tok, "GET ", STRLEN("GET ")) == 0)
               url = tok + STRLEN("GET ");
            else if (strncasecmp(tok, "HEAD ", STRLEN("HEAD ")) == 0)
               url = tok + STRLEN("HEAD ");
            else assert(0);
         }
         //d += sprintf(d, "%s %s HTTP/1.%c"NL, a100 || !get ? "GET":get, url, proxy.httpversion ? proxy.httpversion:'0');
         d += sprintf(d, "GET %s HTTP/1.%c"NL, url, proxy.httpversion ? proxy.httpversion:'0');

      } else if (strncasecmp(tok, "Host:", STRLEN("Host:")) == 0) {
         if (proxy.port == 80)
            d += sprintf(d, "Host: %s"NL, proxy.host);
         else
            d += sprintf(d, "Host: %s:%d"NL, proxy.host, proxy.port);
      } else if (strncasecmp(tok, "User-Agent:", STRLEN("User-Agent:")) == 0 && proxy.useragent[0]) {
         d += sprintf(d, "User-Agent: %s"NL, proxy.useragent);
      } else {
         d += sprintf(d, "%s"NL, tok);
         assert(d < response_out + MAX_PROXY_BUFFER);
      }
      if (next_tok = strstr(last_tok, NL), next_tok) tok = last_tok, *next_tok = '\0', last_tok = next_tok + strlen(NL);
      else tok = NULL;
   }
/* 855
wget         proxy            google
####################################### socketread
GET>
             <GET
####################################### (sendwithresponse)
             GET>
                              <GET
                              >OK
             OK<
####################################### (simpleresponse)
             >OK
OK<
*/
   if (body_size != 0) {
      error_printf(LOG_PROXY, "header: unexpected body_size = %d\n", body_size);
      goto fail;
   }
   sockfd = make_socket_out(LOG_PROXY, proxy.sin_addr, proxy.port);
   if (sockfd < 0) {
      error_printf(LOG_PROXY, "header: make_socket_out = %d\n", sockfd);
      goto fail;
   }
   s = socket_write(LOG_PROXY, sockfd, response_out, strlen(response_out));
   if (s != strlen(response_out))  {
      error_printf(LOG_PROXY, "header: socket_write(%d) %d != %d\n", sockfd, s, strlen(response_out));
      goto fail;
   }
   s = wait_for_socket(LOG_PROXY, sockfd, 1000000);
   if (s <= 0) {
      error_printf(LOG_PROXY, "header: wait_for_socket(%d)=%d\n", sockfd, s);
      goto fail;
   }
   nbytes = http_read_header(LOG_PROXY, sockfd, buffer, MAX_PROXY_BUFFER, &body, &body_size);
   s = socket_write(LOG_PROXY, filedes, buffer, nbytes);
   if (s != nbytes) {
      error_printf(LOG_PROXY, "header: socket_write(%d) %d != %d\n", filedes, s, nbytes);
      goto fail;
   }
   const int header_length = body_size && body ? body-buffer:nbytes;
   int remaining = header_length + body_size - nbytes;
   while (remaining > 0) {
      s = wait_for_socket(LOG_PROXY, sockfd, 1000000);
      if (s <= 0) {
         error_printf(LOG_PROXY, "content: wait_for_socket(%d)=%d\n", sockfd, s);
         goto fail;
      }
      int nbytes = socket_read(LOG_PROXY, sockfd, buffer, min(remaining, MAX_PROXY_BUFFER));
      if (nbytes < 0) {
         error_printf(LOG_PROXY, "content: socket_read(%d) %d != %d\n", sockfd, nbytes, min(remaining, MAX_PROXY_BUFFER));
         goto fail;
      } else if (nbytes > 0) {
         s = socket_write(LOG_PROXY, filedes, buffer, nbytes);
         if (s != nbytes) {
            error_printf(LOG_PROXY, "read_from_proxy(%d) sent %d/%d (%d remaining)\n", filedes, s, nbytes, remaining);
            goto fail;
         }
         remaining -= nbytes;
      }
      //fprintf(stderr, "%d/%d\n", nbytes, remaining);
   }
   if (remaining != 0) {
      log_printf(LOG_PROXY, "content: remaining=%d (%d=%d-%d) (%d=%d+%d-%d)\n", remaining, header_length, body, buffer, remaining, header_length, body_size, nbytes);
      error_printf(LOG_PROXY, "content: remaining=%d (%d=%d-%d) (%d=%d+%d-%d)\n", remaining, header_length, body, buffer, remaining, header_length, body_size, nbytes);
      goto fail;
   }
   status = STATUS_OK; // normal exit
   fail:
   if (sockfd >= 0) close(sockfd);
   if (buffer) free(buffer);
   if (response_out) free(response_out);
   return status;
}

static void *proxy_thread(void *arg)
{
   int i, s;
   fd_set active_fd_set, read_fd_set;
   /* Create the socket and set it up to accept connections. */
   int sock = make_socket_in(LOG_PROXY, PROXYPORT);
   if (sock < 0) {
      error_printf(LOG_PROXY, "proxy_thread: Failed to create socket: sock=%i\n", sock);
      return 0;
   }
   log_printf(LOG_PROXY, "proxy_thread. sock=%i\n", sock);

   /* Initialize the set of active sockets. */
   FD_ZERO(&active_fd_set);
   FD_SET(sock, &active_fd_set);
   while (1) {
      /* Block until input arrives on one or more active sockets. */
      read_fd_set = active_fd_set;
      if (s = select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL), s < 0) {
         error_printf(LOG_PROXY, "proxy_thread: select(%d)=%i\n", read_fd_set, s);
         break;
      }
      //printf("select returns %i\n", s);
      /* Service all the sockets with input pending. */
      for (i = 0; i < FD_SETSIZE; i++) {
         if (FD_ISSET(i, &read_fd_set)) {
            if (i == sock) {
               /* Connection request on original socket. */
               size_t size = sizeof clientname_proxy;
               int new_sock = accept(sock, (struct sockaddr *)&clientname_proxy, &size);
               if (new_sock < 0) {
                   error_printf(LOG_PROXY, "proxy_thread: accept(%d)=%i\n", sock, new_sock);
                   continue;
               }
               log_printf(LOG_PROXY, "%i) Proxy: connect from (%s:%d)=%d\n", i, inet_ntoa(clientname_proxy.sin_addr), ntohs(clientname_proxy.sin_port), new_sock);
               FD_SET(new_sock, &active_fd_set);
            } else {
               int s = read_from_proxy(i);
               log_printf(LOG_PROXY, "%i) read_from_proxy=%d\n", i, s);
               /* Data arriving on an already-connected socket. */
               //fprintf (stderr, "%i) Server: connect from existing socket\n", i);
               if (s < 0) {
                  //fprintf (stderr, "%i) Server: connect from existing socket closed\n", i);
                  if (s = close(i), s != 0) {
                      error_printf(LOG_PROXY, "proxy_thread: close(%d)=%i\n", i, s);
                      continue;
                  }
                  FD_CLR(i, &active_fd_set);
               }
            }
         }
      }
   }
}

int main (int argc, char *argv[])
{
   (void) signal(SIGINT, ex_program);
   (void) signal (SIGSEGV ,ex_program);
   (void) signal (SIGPIPE, ignore_signal);
   int i, s;
   int sock;
   fd_set active_fd_set, read_fd_set;
   pthread_t proxy_threadt;

   for (i=1; i<argc; i++)   
      if (argv[i][0] == '-' && argv[i][1] == 'v') loglevel = 2;
      else if (argv[i][0] == '-' && argv[i][1] == 'p') proxyon = 1;
      else if (argv[i][0] == '-' && argv[i][1] == 'q') proxyoff = 1;

   log_printf(LOG_MAIN, "%s: loglevel=%d, proxyon=%d, proxyoff=%d\n", argv[0], loglevel, proxyon, proxyoff);
   s = pthread_create( &proxy_threadt, NULL, proxy_thread, NULL);
   assert(s==0);

   /* Create the socket and set it up to accept connections. */
   sock = make_socket_in(LOG_MAIN, PORT);
   if (sock < 0) {
      error_printf(LOG_MAIN, "main: Failed to create socket: sock=%i\n", sock);
      return -1;
   }

   /* Initialize the set of active sockets. */
   FD_ZERO(&active_fd_set);
   FD_SET(sock, &active_fd_set);

   while (1) {
      /* Block until input arrives on one or more active sockets. */
      read_fd_set = active_fd_set;
      if (s = select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL), s < 0) {
          error_printf(LOG_MAIN, "main: select(%d)=%i\n", read_fd_set, s);
          break;
      }
      //printf("select returns %i\n", s);
      /* Service all the sockets with input pending. */
      for (i = 0; i < FD_SETSIZE; ++i) {
         if (FD_ISSET (i, &read_fd_set)) {
            if (i == sock) {
               /* Connection request on original socket. */
               size_t size = sizeof clientname;
               int new_sock = accept(sock, (struct sockaddr *)&clientname, &size);
               if (new_sock < 0) {
                   error_printf(LOG_MAIN, "main: accept(%d)=%i\n", sock, new_sock);
                   continue;
               }
               log_printf(LOG_MAIN, "%i) Server: connect from (%s:%d)=%d\n", i, inet_ntoa(clientname.sin_addr), ntohs(clientname.sin_port), new_sock);
               FD_SET(new_sock, &active_fd_set);
            } else {
               /* Data arriving on an already-connected socket. */
               log_printf(LOG_MAIN, "%i) Server: connect from existing socket\n", i);
               if (read_from_client(i) < 0) {
                  //fprintf(stderr, "%i) Server: connect from existing socket closed\n", i);
                  if (s = close(i), s != 0) {
                      error_printf(LOG_MAIN, "main: close(%d)=%i\n", i, s);
                      continue;
                  }
                  FD_CLR(i, &active_fd_set);
               }
            }
         }
      }
   }
}

