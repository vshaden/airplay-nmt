#include <stdio.h>
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

#define PORT      6000
#define PROXYPORT 7000
#define MAX_HEADER   5120
#define MAX_RESPONSE 5120
#define MAX_SEND 5120
#define NL "\r\n"

#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

static struct sockaddr_in clientname, clientname_proxy; // hack: make global

enum {
   STATUS_OK=200, STATUS_SWITCHING_PROTOCOLS=101, STATUS_NOT_IMPLEMENTED=501,
};

static char airplay_url[MAX_SEND];

static int loglevel=1;

#define log_printf if (loglevel<2) {} else printf
#define error_printf if (loglevel<1) {} else printf
#define STRLEN(s) (sizeof(s)-1)
#define perror_abort(s) do {perror(s); exit(EXIT_FAILURE);}while(0)

static char *UrlEncode(char *szText, char* szDst, int bufsize, int want_proxy)
{
   char ch; 
   char szHex[5];
   int iMax,i,j; 

   iMax = bufsize-2;
   szDst[0]='\0';
   for (i = 0,j=0; szText[i] && j <= iMax; i++) {
      ch = szText[i];
      if (want_proxy && strncmp(szText+i, "http://", STRLEN("http://"))==0) {
         strcpy(szDst+j, "http://127.0.0.1:7000/proxy?url=http");
         j += STRLEN("http://127.0.0.1:7000/proxy?url=http");
         i += STRLEN("http")-1;
      } else if (strncmp(szText+i, "://0.0.0.0", STRLEN("://0.0.0.0"))==0) {
         strcpy(szDst+j, "://");
         j += STRLEN("://");
         strcpy(szDst+j, inet_ntoa(clientname.sin_addr));
         j += strlen(inet_ntoa(clientname.sin_addr));
         i += STRLEN("://0.0.0.0")-1;
      } else if (isalnum(ch) || ch=='/' || ch==':' || ch=='.' || ch=='_' || ch=='-')
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

static char *strnstr(const char *s1, const char *s2, size_t n)
{
   char safe = s1[n-1];
   ((char *)s1)[n-1] = '\0';
   char *s = strstr(s1, s2);
   ((char *)s1)[n-1] = safe;
   return s;
}

/* reads http header until response_size, NL NL or EOF.
   may have read some of body, which will be pointed to by body (first byte after NL NL)
   returns bytes read into response, or negative for error
*/
static int http_read_header(int sockfd, char *response, int response_size, char **body, int *body_size)
{
   int bytes_read=0;
   if (!response) return 0;
   if (body) *body = NULL;
   if (body_size) *body_size = 0;
   while (1) {
      int nbytes = read(sockfd, response+bytes_read, response_size-bytes_read);
      //error_printf("% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, response+bytes_read);
      //printf("http_read_header: read=%d, %d/%d\n", nbytes, bytes_read, response_size);
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
   log_printf("http_read_header(%d): DONE %d/%d\n[%.*s]...%d\n", sockfd, bytes_read, response_size, header_size, response, body_size ? *body_size:0 );
   return bytes_read;
}


static int socket_read(int sockfd, char *buffer, int length)
{
   int bytes_read=0;
   int remaining = length;
   if (buffer) while (1) {
         int nbytes = read(sockfd, buffer+bytes_read, remaining);
         //error_printf("% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, response+bytes_read);
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
   if (bytes_read < 4096) log_printf("socket_read(%d): DONE %d/%d (%d)\n[%.*s]\n", sockfd, bytes_read, length, remaining, bytes_read, buffer);
   else log_printf("socket_read(%d): DONE %d/%d (%d)\n", sockfd, bytes_read, length, remaining);
   return bytes_read;
}

static int socket_write(int sockfd, const char *buffer, int length)
{
   int bytes_written=0;
   int remaining = length;
   if (buffer) while (1) {
         int nbytes = write(sockfd, buffer+bytes_written, remaining);
         //error_printf("% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, buffer+bytes_written);
         //printf("socket_write: read=%d, %d/%d\n", nbytes, bytes_read, response_size);
         if (nbytes < 0) {
            /* Write error. */
            if (bytes_written < length) {
               error_printf("socket_write(%d)=%d: DONE %d/%d (%d)\n", sockfd, nbytes, bytes_written, length, remaining);
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
   if (bytes_written < 4096) log_printf("socket_write(%d): DONE %d/%d (%d)\n[%.*s]\n", sockfd, bytes_written, length, remaining, bytes_written, buffer);
   else log_printf("socket_write(%d): DONE %d/%d (%d)\n", sockfd, bytes_written, length, remaining);
   return bytes_written;
}

static void http_response(int sockfd, int status, const char *status_string, const char *content)
{
   char response[MAX_RESPONSE];
   const time_t ltime=time(NULL); /* get current cal time */
   const char *date = asctime(gmtime(&ltime)); //"Fri, 17 Dec 2010 11:18:01 GMT"NL;
   if (content && content[0] == '*') content++, sprintf(response, "HTTP/1.1 %d %s"NL"Date: %s", status, status_string, date);
   else sprintf(response, "HTTP/1.1 %d %s"NL "Date: %s" "Content-Length:%d"NL, status, status_string, date, content ? strlen(content)+2:0);
   if (content) strcat(response, ""NL), strcat(response, content);
   strcat(response, ""NL);
   //if (status != STATUS_SWITCHING_PROTOCOLS) {log_printf("{%s}\n", response);}
   int s = socket_write(sockfd, response, strlen(response));
   assert(s==strlen(response));
   //log_printf("http_response(%d): DONE (%d)\n[%.*s]\n", sockfd, s, s, content);
}


static int make_socket_out(struct in_addr sin_addr, int port)
{
   struct sockaddr_in  server;
   int s, sockfd = socket(AF_INET,SOCK_STREAM,0);

   if (sockfd==-1) perror_abort("Create socket");
   /*
    * copy the network address part of the structure to the 
    * sockaddr_in structure which is passed to connect() 
    */
   memcpy(&server.sin_addr, &sin_addr, sizeof server.sin_addr);
   server.sin_family = AF_INET;
   server.sin_port = htons(port);

   /* connect */
   if (s = connect(sockfd, (struct sockaddr *)&server, sizeof server), s) {
      perror_abort("error connecting");
   }
   log_printf("make_socket_out(%d.%d.%d.%d:%d)=%d\n", (sin_addr.s_addr>>0)&0xff,(sin_addr.s_addr>>8)&0xff,(sin_addr.s_addr>>16)&0xff,(sin_addr.s_addr>>24)&0xff, port, sockfd);
   return sockfd;
}


int sendCommandGetResponse(struct in_addr sin_addr, int port, const char *cmd, char *response, int response_size)
{
   int sockfd=make_socket_out(sin_addr, port);
   int s = socket_write(sockfd, cmd, strlen(cmd));
   assert(s == strlen(cmd));
   int bytes_read = socket_read(sockfd, response, response_size);
   close(sockfd);
   response[min(bytes_read, response_size-1)] = '\0';
   return 0;
}


#if 0
static int sendCommand(int port, const char *cmd)
{
   struct in_addr sin_addr;
   struct hostent     *he;
   if ((he = gethostbyname("localhost")) == NULL) {
      perror_abort("error resolving hostname");
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);
   return sendCommandGetResponse(sin_addr, port, cmd, NULL, 0);
}
#endif

static void ignore_signal(int sig) {
   error_printf("Ignored signal %d\n", sig);
}

static void ex_program(int sig) {
#if 1
   error_printf("Wake up call ... !!! - Caught signal: %d ... !!\n", sig);
   fflush(stdout);
   fflush(stderr);
   exit(EXIT_FAILURE);
#else
   close (sock);
   FD_CLR (sock, &active_fd_set);
   (void) signal(SIGINT, SIG_DFL);
#endif
}

static int http_request_with_response(const char *url, char *response, int response_size)
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
   log_printf("parsed [%s] to [%s] %d (%d)\n", url, host, port, s);

   struct hostent     *he;
   if ((he = gethostbyname(host)) == NULL) {
      perror_abort("error resolving hostname..");
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);
   sprintf(command, 
           "GET %.*s HTTP/1.1"NL
           "User-Agent: Wget/1.11.2"NL
           "Accept: */*"NL
           "Host: %.*s"NL
           "Connection: Keep-Alive"NL
           , url+strlen(url)-p, p, p-url, url);
   return sendCommandGetResponse(sin_addr, port, command, response, response_size);
}

static int http_request_and_parse_nums(const char *command, const char *key[], int *value)
{
   char response[MAX_RESPONSE];
   int s = http_request_with_response(command, response, sizeof response);
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

#if 0
static int http_request_and_parse_num(const char *command, const char *key, int *value)
{
   const char *keys[2] = {key, NULL};
   return http_request_and_parse_nums(command, keys, value);
}
#endif

static int http_request(const char *command)
{
   char response[MAX_RESPONSE];
   int s = http_request_with_response(command, response, sizeof response);
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
   return 1;
}
static int get_photo_info(int *position, int *duration, int *playing, int *paused, int *stopped, int *buffering, int *seekable)
{
   if (position) *position =0;
   if (duration) *duration =0;
   if (playing)  *playing  =1;
   if (paused)   *paused   =0;
   if (stopped)  *stopped  =0;
   if (buffering)*buffering=0;
   if (seekable) *seekable =0;
   return 1;
}

static int get_system_mode(int *browser, int *pod_playback, int *vod_playback)
{
   if (browser) *browser=1;
   if (pod_playback) *pod_playback=0;
   if (vod_playback) *vod_playback=0;
   return 1;
}
#else
static int get_media_info(int *position, int *duration, int *playing, int *paused, int *stopped, int *buffering, int *seekable)
{
   const char *keys[] = {"?<returnValue>0", "<currentTime>", "<totalTime>", "?<currentStatus>play", "?<currentStatus>pause", "?<currentStatus>stop", "?<currentStatus>buffering", "?<seekEnable>true", NULL};
   int values[countof(keys)]={0};
   http_request_and_parse_nums("http://127.0.0.1:8008/playback?arg0=get_current_vod_info", keys, values);
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
   http_request_and_parse_nums("http://127.0.0.1:8008/playback?arg0=get_current_pod_info", keys, values);
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
   http_request_and_parse_nums("http://127.0.0.1:8008/system?arg0=get_current_app", keys, values);
   if (browser) *browser=values[1];
   if (pod_playback) *pod_playback =values[2];
   if (vod_playback) *vod_playback=values[3];
   return !values[0];
}
#endif
#ifdef A100
static void send_ir_key(char *str)
{
   FILE *fp = fopen("/tmp/ir_key", "wb");
   if (fp) {
      fprintf (fp ,"%s\n", str);
      fclose(fp);
      log_printf("Wrote %s to /tmp/ir_key\n", str);
   } else assert(0);
}
#else
static void send_ir_key(char *str)
{
   int s, sockfd;
   struct in_addr sin_addr;
   struct hostent     *he;
   if ((he = gethostbyname("localhost")) == NULL) {
      perror_abort("error resolving hostname");
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);

   sockfd = make_socket_out(sin_addr, 30000);
   if (sockfd > 0) {
      s = socket_write(sockfd, str, strlen(str));
      assert(s == strlen(str));
      close(sockfd);
   } else assert(0);
}
#endif

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
            log_printf("************ media_status = %d ****************\n", media_status);
            stopped = 1;
         }
      } else {
         media_status = get_media_info(position, duration, &playing, &paused, &stopped, &buffering, &seekable);
         if (media_status!=0) {
            log_printf("************ media_status = %d ****************\n", media_status);
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
         if (playing && !buffering && seekable)
            break;
      } else if (mode == MEDIA_PHOTO) {
         if (playing)
            break;
      }
      if (timeout++ == 250) {
         log_printf("TIMEOUT: wait_media_ready(%s->%s, %d, %d, %d) failed (%d,%d,%d,%d,%d,%d,%d)\n", 
                    modename[last_mode], modename[mode], seek_offset, *position, *duration,
                    seekable, playing, paused, stopped, buffering, 0, media_status );
         break;
      }
      usleep(10000); // 0.1s
   }
   log_printf("wait_media_ready(%s->%s, %d, %d, %d) okay (%d,%d,%d,%d,%d,%d,%d)\n", 
              modename[last_mode], modename[mode], seek_offset, *position, *duration,
              seekable, playing, paused, stopped, buffering, 0, media_status );
   return media_status;
}

static int last_scrub=0, last_position=0, last_duration = 0;
#ifndef A100
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
   log_printf("### set_media_mode_ex(%s,%d)\n", modename[mode], seek_offset);
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
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=start_vod&arg1=AirPlayVideo&arg2=%s&arg3=show&arg4=%d", url, 0);
      //sprintf(cmd, "http://127.0.0.1:8080/xbmcCmds/xbmcHttp?command=PlayFile(%s)", url);
      break;
   case MEDIA_PHOTO:
      send_command = 1;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=start_pod&arg1=AirPlayPhoto&arg2=%s&arg3=1&arg4=rot0&arg5=bghide", url);
      break;
   case MEDIA_SEEK:
      assert(seek_offset >= 0);
      send_command = 1;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=set_time_seek_vod&arg1=%02d:%02d:%02d", seek_offset / 3600, (seek_offset / 60)%60, seek_offset % 60);
      break;
   case MEDIA_PAUSE:
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=pause_vod");
      break;
   case MEDIA_RESUME:
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=resume_vod");
      break;
   }
   //if (last_mode == MEDIA_STOP && mode != MEDIA_PLAY) send_command = 0;
   if (send_command) {
      s = http_request(cmd);
      assert(s==0);
   }
   media_status = wait_media_ready(mode, last_mode, seek_offset, position, duration);
   if (mode==MEDIA_PHOTO) {
      int browser, pod_playback, vod_playback;
      s = get_system_mode(&browser, &pod_playback, &vod_playback);
      assert(s==0);
      if (!pod_playback) {
         usleep(100000);
         send_ir_key("B\n");
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
         send_ir_key("xx");
      }
   }
   if (mode==MEDIA_STOP) {
      last_scrub =-1; last_position = 0, last_duration = 0;
   }
   last_mode = media_status==0 ? mode : MEDIA_STOP;
   return media_status;
}
#else
static int set_media_mode_ex(MEDIA_MODES_T mode, const char *url, int seek_offset, int *position, int *duration)
{
   int s;
   FILE *fp;
   char cmd[MAX_SEND];
   log_printf("### set_media_mode_ex(%s,%d)\n", modename[mode], seek_offset);
   switch (mode) {
   default: break;
   case MEDIA_STOP:
      send_ir_key("27");
      break;
   case MEDIA_PAUSE:
      send_ir_key("234");
      break;
   case MEDIA_RESUME:
      send_ir_key("233");
      break;
   case MEDIA_PLAY:
   case MEDIA_PHOTO:
#if 0
      sprintf(cmd,"/bin/mono -single %s -dram 1\n", url);
      system(cmd);
#else
      fp = fopen("/tmp/runme.html", "wb");
      if (fp) {
         fprintf (fp ,"<body bgcolor=black link=black onloadset='go'>");
         fprintf (fp ,"<a onfocusload name='go' href='%s' %s></a>", url, mode==MEDIA_PLAY?"vod":"");
         fprintf (fp ,"<a href='http://127.0.0.1:8883/start.cgi?list' tvid='home'></a>");
         fprintf (fp ,"<a href='http://127.0.0.1:8883/start.cgi?list' tvid='source'></a>");
         fprintf (fp ,"<br><font size='6' color='#ffffff'><b>Press Return on your remote to go back to your previous location</b></font>\n");
         fclose(fp);
      }
      if (fp) {
         fp = fopen("/tmp/gaya_bc", "wb");
      }
      if (fp) {
         fprintf (fp, "/tmp/runme.html");
         fclose(fp);
         log_printf("Wrote to /tmp/gaya_bc\n");
      }
      if (!fp) {
         log_printf("failed to open /tmp/runme.html\n");
      }
#endif
      break;
   }
   return 0;
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


static int make_socket_in(uint16_t port)
{
   struct sockaddr_in name;
   int reuse_addr = 1;

   /* Create the socket. */
   int sock = socket(PF_INET, SOCK_STREAM, 0);
   if (sock < 0) {
      perror_abort("socket");
   }
   setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

   /* Give the socket a name. */
   memset((char *) &name, 0, sizeof name);
   name.sin_family = AF_INET;
   name.sin_port = htons (port);
   name.sin_addr.s_addr = htonl (INADDR_ANY);
   if (bind(sock, (struct sockaddr *) &name, sizeof name) < 0) {
      perror_abort("bind");
   }
   if (listen(sock, 1) < 0) {
      perror_abort("listen");
   }
   log_printf("make_socket_in(:%d)=%d\n", port, sock);
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
   int nbytes;
   char *found;
   int status = STATUS_OK;
   char *body=0; int body_size = 0;
   assert(buffer && content);
   content[0] = '\0';
   nbytes = http_read_header(filedes, buffer, MAX_HEADER, &body, &body_size);
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
   } else if (found = strstr(buffer,"/play"), found) {
#ifdef A100
      int itunes = 1;
#else
      int itunes = 0;
#endif
      found = strstr(buffer, "User-Agent: ");
      if (found) {
         found += STRLEN("User-Agent: ");
         if (strncmp(found, "iTunes", STRLEN("iTunes"))==0)
            itunes=1;
      }
      found = strstr(buffer, "Content-Location: ");
      if (found) {
         found += STRLEN("Content-Location: ");
         char *newline = strchr(found, '\n');
         if (newline) *newline = '\0';
         else found = 0;
      }
      if (found && !media_supported(found)) found = 0;
      if (found) {
         fprintf (stderr, "Found content: %s\n", found);
         UrlEncode(found, airplay_url, sizeof airplay_url, itunes);
         set_media_mode_url(MEDIA_PLAY, airplay_url);
         if (last_scrub > 30) { // don't bother seeking small distances
            set_media_mode_ex(MEDIA_SEEK, NULL, last_scrub, NULL, NULL);
            last_position = last_scrub;
            last_scrub = -1;
         }
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
      //set_media_mode_ex(MEDIA_SEEK, NULL, seek_offset, &position, &duration );
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
            last_duration = duration;
            last_position = position;
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
         s = socket_read(filedes, photo+already_got, body_size-already_got);
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
   } else {
      fprintf (stderr, "Unhandled [%s]\n", buffer);
      status = STATUS_NOT_IMPLEMENTED;
   }
   if (status) http_response(filedes, status, http_status_string(status), content[0] ? content:NULL);
   fail:
   if (buffer) free(buffer);
   if (content) free(content);
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
   char *buffer = malloc(MAX_HEADER+1); // allow null on end
   char *response_out = malloc(MAX_HEADER);
   int nbytes;
   char *found, *p;
   int status = STATUS_OK;
   char *body=0; int body_size = 0;
   assert(buffer);
   memset(&proxy, 0, sizeof proxy);
   nbytes = http_read_header(filedes, buffer, MAX_HEADER, &body, &body_size);
   if (nbytes > 0) {
      buffer[nbytes] = '\0';
      assert(body_size == 0);
   } else {
      /* End-of-file. */
      assert(nbytes == 0);
      status = 1;
      goto fail;
   }
   char *tok = buffer, *last_tok = buffer, *next_tok = NULL;
   char *d = response_out;
   if (next_tok = strstr(last_tok, NL), next_tok) tok = last_tok, *next_tok = '\0', last_tok = next_tok + strlen(NL);
   else tok = NULL;
   while (tok) {
      if (strncasecmp(tok, "GET ", STRLEN("GET ")) == 0 || strncasecmp(tok, "HEAD ", STRLEN("HEAD ")) == 0 ) {
         char *url;
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
                  log_printf("parsed [%s] (%d.%d.%d.%d:%d)\n", proxy.host, (proxy.sin_addr.s_addr>>0)&0xff,(proxy.sin_addr.s_addr>>8)&0xff,(proxy.sin_addr.s_addr>>16)&0xff,(proxy.sin_addr.s_addr>>24)&0xff, proxy.port);
                  perror_abort("error resolving hostname");
               }
               memcpy(&proxy.sin_addr, he->h_addr_list[0], sizeof proxy.sin_addr);
               log_printf("parsed [%s] (%d.%d.%d.%d:%d)\n", proxy.host, (proxy.sin_addr.s_addr>>0)&0xff,(proxy.sin_addr.s_addr>>8)&0xff,(proxy.sin_addr.s_addr>>16)&0xff,(proxy.sin_addr.s_addr>>24)&0xff, proxy.port);
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
               log_printf("parsed useragent [%s]\n", proxy.useragent);
            }
         } else {
            if (strncasecmp(tok, "GET ", STRLEN("GET ")) == 0)
               url = tok + STRLEN("GET ");
            else if (strncasecmp(tok, "HEAD ", STRLEN("HEAD ")) == 0)
               url = tok + STRLEN("HEAD ");
            else assert(0);
         }
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
         assert(d < response_out + MAX_HEADER);
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
   assert(body_size == 0);
   int s, sockfd = make_socket_out(proxy.sin_addr, proxy.port);
   s = socket_write(sockfd, response_out, strlen(response_out));
   assert(s == strlen(response_out));

   nbytes = http_read_header(sockfd, buffer, MAX_HEADER, &body, &body_size);
   s = socket_write(filedes, buffer, nbytes);
   assert(s == nbytes);
   const int header_length = body-buffer;
   int remaining = header_length + body_size - nbytes;
   while (remaining) {
      int nbytes = socket_read(sockfd, buffer, min(remaining, MAX_HEADER));
      assert(nbytes >= 0);
      if (nbytes > 0) {
         s = socket_write(filedes, buffer, nbytes);
         if (s != nbytes) {
            error_printf("read_from_proxy(%d) sent %d/%d (%d remaining)\n", filedes, s, nbytes, remaining);
            remaining = 0;
            break;
         }
         remaining -= nbytes;
      }
      //fprintf(stderr, "%d/%d\n", nbytes, remaining);
   }
   assert(remaining == 0);
   close(sockfd);
   status = 1;
   fail:
   if (buffer) free(buffer);
   if (response_out) free(response_out);
   return status==STATUS_OK || status==STATUS_SWITCHING_PROTOCOLS ? 0:-status;
}

static void *proxy_thread(void *arg)
{
   int i, s;
   fd_set active_fd_set, read_fd_set;
   /* Create the socket and set it up to accept connections. */
   int sock = make_socket_in(PROXYPORT);

   log_printf("proxy_thread. sock=%i\n", sock);

   /* Initialize the set of active sockets. */
   FD_ZERO(&active_fd_set);
   FD_SET(sock, &active_fd_set);
   while (1) {
      /* Block until input arrives on one or more active sockets. */
      read_fd_set = active_fd_set;
      if (s = select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL), s < 0) {
         perror_abort("select");
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
                  perror_abort("accept");
               }
               log_printf("%i) Proxy: connect from (%s:%d)=%d\n", i, inet_ntoa(clientname_proxy.sin_addr), ntohs(clientname_proxy.sin_port), new_sock);
               FD_SET(new_sock, &active_fd_set);
            } else {
               /* Data arriving on an already-connected socket. */
               //fprintf (stderr, "%i) Server: connect from existing socket\n", i);
               if (read_from_proxy(i) < 0) {
                  //fprintf (stderr, "%i) Server: connect from existing socket closed\n", i);
                  close(i);
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

   if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v') loglevel = 2;

   s = pthread_create( &proxy_threadt, NULL, proxy_thread, NULL);
   assert(s==0);

   /* Create the socket and set it up to accept connections. */
   sock = make_socket_in(PORT);

   /* Initialize the set of active sockets. */
   FD_ZERO(&active_fd_set);
   FD_SET(sock, &active_fd_set);

   while (1) {
      /* Block until input arrives on one or more active sockets. */
      read_fd_set = active_fd_set;
      if (s = select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL), s < 0) {
         perror_abort("select");
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
                  perror_abort("accept");
               }
               log_printf("%i) Server: connect from (%s:%d)=%d\n", i, inet_ntoa(clientname.sin_addr), ntohs(clientname.sin_port), new_sock);
               FD_SET(new_sock, &active_fd_set);
            } else {
               /* Data arriving on an already-connected socket. */
               log_printf("%i) Server: connect from existing socket\n", i);
               if (read_from_client(i) < 0) {
                  //fprintf(stderr, "%i) Server: connect from existing socket closed\n", i);
                  close(i);
                  FD_CLR(i, &active_fd_set);
               }
            }
         }
      }
   }
}

