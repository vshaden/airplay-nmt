#include <stdio.h>
#include <assert.h>
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

#define PORT    6000
#define MAXMSG  512000
#define MAX_RESPONSE 5120
#define MAX_SEND 5120
#define NL "\r\n"

static struct sockaddr_in clientname; // hack: make global


enum {
   STATUS_OK=200, STATUS_SWITCHING_PROTOCOLS=101, STATUS_NOT_IMPLEMENTED=501,
};

static char airplay_url[MAX_SEND];

static int loglevel=1;

#define log_printf if (loglevel<2) {} else printf
#define error_printf if (loglevel<1) {} else printf

char * UrlEncode(char *szText, char* szDst, int bufsize) {
   char ch; 
   char szHex[5];
   int iMax,i,j; 

   iMax = bufsize-2;
   szDst[0]='\0';
   for (i = 0,j=0; szText[i] && j <= iMax; i++) {
      ch = szText[i];
      if (strncmp(szText+i, "://0.0.0.0", sizeof("://0.0.0.0")-1)==0) {
	strcpy(szDst+j, "://");
        j += strlen("://");
	strcpy(szDst+j, inet_ntoa(clientname.sin_addr));
        j += strlen(inet_ntoa(clientname.sin_addr));
        i += strlen("://0.0.0.0")-1;
      } else if (isalnum(ch))
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
void http_response(int filedes, int status, const char *status_string, const char *content)
{
   static char response[MAX_RESPONSE];
   const time_t ltime=time(NULL); /* get current cal time */
   const char *date = asctime(gmtime(&ltime)); //"Fri, 17 Dec 2010 11:18:01 GMT"NL;
   if (content && content[0] == '*') content++, sprintf(response, "HTTP/1.1 %d %s"NL"Date: %s", status, status_string, date);
   else sprintf(response, "HTTP/1.1 %d %s"NL "Date: %s" "Content-Length:%d"NL, status, status_string, date, content ? strlen(content)+2:0);
   if (content) strcat(response, ""NL), strcat(response, content);
   strcat(response, ""NL);
   if (status != STATUS_SWITCHING_PROTOCOLS) log_printf("{%s}\n", response);
   int s = write (filedes, response, strlen(response));
   assert(s==strlen(response));
}


static fd_set active_fd_set, read_fd_set;

static int socket_read(int sockfd, char *response, int response_size, char **body, int *body_length)
{
   int bytes_read=0;
   int content_length = -1;
   int remaining = response_size;
   if (response) while (1) {
         int nbytes = read(sockfd, response+bytes_read, remaining);
         //error_printf("% 6d: [%.*s]\n", nbytes, nbytes < 0 ? 0:nbytes, response+bytes_read);
         //printf("socket_read: read=%d, %d/%d\n", nbytes, bytes_read, response_size);
         if (nbytes < 0) {
            /* Read error. */
            perror ("read");
            exit (EXIT_FAILURE);
         } else if (nbytes == 0) {
            /* End-of-file. */
            break;
         } else {
            /* Data read. */
            bytes_read += nbytes;
            remaining -= nbytes;
            assert(bytes_read < response_size);
            response[bytes_read] = '\0';
            if (content_length < 0) {
               char *newlines, *found;
               if (found = strstr(response, "Content-Length:"), found) newlines = strstr(found, NL NL);
               if (found && newlines) {
                  newlines += strlen(NL NL);
                  content_length = atoi(found + strlen("Content-Length:"));
                  remaining = content_length - (response+bytes_read-newlines);
                  assert(bytes_read+remaining < response_size);
                  if (newlines) {
                     if (body) *body = newlines;
                     if (body_length) *body_length = content_length;
                  }
               }
               //error_printf("socket_read: %d/%d (%d)\n", bytes_read, response_size, remaining);
            }
         }
      }
   //error_printf("socket_read: DONE %d/%d (%d)\n", bytes_read, response_size, remaining);
   return bytes_read;
}

int sendCommandGetResponse(struct in_addr sin_addr, int port, const char *cmd, char *response, int response_size)
{
   struct sockaddr_in  server;
   int sockfd=-1;
   int s;

   sockfd=socket(AF_INET,SOCK_STREAM,0);

   if (sockfd==-1)
      perror("Create socket");

/*
 * copy the network address part of the structure to the 
 * sockaddr_in structure which is passed to connect() 
 */
   memcpy(&server.sin_addr, &sin_addr, sizeof(server.sin_addr));
   server.sin_family = AF_INET;
   server.sin_port = htons(port);

   /* connect */
   if (s = connect(sockfd, (struct sockaddr *)&server, sizeof(server)), s) {
      error_printf("error connecting.. (%d)", s);
      exit(1);
   }
   s = write(sockfd, cmd, strlen(cmd));
   assert(s == strlen(cmd));
   log_printf("*[%s]\n", cmd);

   int bytes_read = socket_read(sockfd, response, response_size, NULL, NULL);
   close(sockfd);
   if (bytes_read > 0) {
      response[bytes_read] = '\0';
      log_printf("*{%s}\n", response);
   }
   return 0;
}

int sendCommand(int port, const char *cmd)
{
   struct in_addr sin_addr;
   struct hostent     *he;
   if ((he = gethostbyname("localhost")) == NULL) {
      puts("error resolving hostname..");
      exit(1);
   }
   memcpy(&sin_addr, he->h_addr_list[0], sizeof sin_addr);
   return sendCommandGetResponse(sin_addr, port, cmd, NULL, 0);
}

void ex_program(int sig) {
#if 1
   error_printf("Wake up call ... !!! - Caught signal: %d ... !!\n", sig);
   fflush(stdout);
   fflush(stderr);
   exit(1);
#else
   close (sock);
   FD_CLR (sock, &active_fd_set);
   (void) signal(SIGINT, SIG_DFL);
#endif
}

static int http_request_with_response(const char *url, char *response, int response_size)
{
   FILE *fp;
   static char command[MAX_SEND];
   static char host[MAX_SEND] = "localhost";
   char *p;
   int port = 80, ip[4];
   int s;
   struct in_addr sin_addr;
   s = sscanf(url, "http://%[^:]s:", host);
   p = strstr(url, "//");
   if (p) p = strstr(p, ":");
   if (p) port = atoi(p+1);
   if (p) p = strstr(p, "/");
   if (!p) return -1;
   //printf("parsed [%s] to [%s] %d (%d)\n", url, host, port, s);

   struct hostent     *he;
   if ((he = gethostbyname(host)) == NULL) {
      puts("error resolving hostname..");
      exit(1);
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
   static char response[MAX_RESPONSE];
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

static int http_request_and_parse_num(const char *command, const char *key, int *value)
{
   const char *keys[2] = {key, NULL};
   return http_request_and_parse_nums(command, keys, value);
}

static int http_request(const char *command)
{
   static char response[MAX_RESPONSE];
   int s = http_request_with_response(command, response, sizeof response);
   return s;
}

typedef enum {
   MEDIA_STOP, MEDIA_PLAY, MEDIA_PAUSE, MEDIA_RESUME, MEDIA_SEEK, MEDIA_PHOTO
} MEDIA_MODES_T;
static const char *modename[] = {"MEDIA_STOP", "MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_RESUME", "MEDIA_SEEK", "MEDIA_PHOTO"};
#define countof(x) (sizeof x/sizeof *(x))
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

static int wait_media_ready(MEDIA_MODES_T mode, MEDIA_MODES_T last_mode, int seek_offset, int *position, int *duration)
{
   static char cmd[MAX_SEND];
   int seekable = 0, playing = 0, paused=0, stopped=0, buffering=0;
   int timeout = 0;
   int media_status;
   assert(position && duration);
   while (1) {
      if (mode == MEDIA_PHOTO)
         media_status = get_photo_info(position, duration, &playing, &paused, &stopped, &buffering, &seekable);
      else
         media_status = get_media_info(position, duration, &playing, &paused, &stopped, &buffering, &seekable);
      if (media_status!=0) {
         log_printf("************ media_status = %d ****************\n", media_status);
         //return media_status;
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
static int set_media_mode_ex(MEDIA_MODES_T mode, const char *url, int seek_offset, int *position, int *duration)
{
   static char cmd[MAX_SEND];
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
      break;
   case MEDIA_PHOTO:
      send_command = 1;
      sprintf(cmd, "http://127.0.0.1:8008/playback?arg0=start_pod&arg1=AirPlayPhoto&arg2=%s&arg3=1&arg4=rot0&arg5=bghide", url);
      //sprintf(cmd, "http://127.0.0.1:8008/system?arg0=send_key&arg2=source&arg3=flashlite");
      //sprintf(cmd, "http://127.0.0.1:8008/system?arg0=send_key&arg2=enter");
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
      if (s!=0) {
         error_printf("http_request(%s)=%d\n", cmd, s);
         exit(1);
      }
   }
   media_status = wait_media_ready(mode, last_mode, seek_offset, position, duration);
   if (mode==MEDIA_STOP) {
      last_scrub =-1; last_position = 0, last_duration = 0;
   }
   last_mode = media_status==0 ? mode : MEDIA_STOP;
   return media_status;
}

static int set_media_mode_url(MEDIA_MODES_T mode, const char *url)
{
   return set_media_mode_ex(mode, url, -1, NULL, NULL);
}

static int set_media_mode(MEDIA_MODES_T mode)
{
   return set_media_mode_ex(mode, NULL, -1, NULL, NULL);
}


static int make_socket (uint16_t port)
{
   //int sock;
   struct sockaddr_in name;

   /* Create the socket. */
   int sock = socket (PF_INET, SOCK_STREAM, 0);
   if (sock < 0) {
      perror ("socket");
      exit (EXIT_FAILURE);
   }

   /* Give the socket a name. */
   name.sin_family = AF_INET;
   name.sin_port = htons (port);
   name.sin_addr.s_addr = htonl (INADDR_ANY);
   if (bind (sock, (struct sockaddr *) &name, sizeof (name)) < 0) {
      perror ("bind");
      exit (EXIT_FAILURE);
   }

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
}

static int read_from_client (int filedes)
{
   static char buffer[MAXMSG];
   static char content[MAX_RESPONSE];
   int nbytes;
   void *found;
   int status = STATUS_OK;
   char *body=0; int body_length = 0;
   content[0] = '\0';
   nbytes = socket_read(filedes, buffer, sizeof buffer, &body, &body_length);
   if (nbytes > 0) {
      buffer[nbytes] = '\0';
      if (strstr(buffer, "/reverse")==0) log_printf("[%s]\n", buffer);
   } else {
      /* End-of-file. */
      assert(nbytes == 0);
      return -1;
   }
   if (found = strstr(buffer,"/reverse"), found) {
      status = STATUS_SWITCHING_PROTOCOLS;
      sprintf(content, "*Upgrade: PTTH/1.0"NL"Connection: Upgrade"NL);
   } else if (found = strstr(buffer,"/rate"), found) {
      found = strstr(found, "?value=");
      int rate = found ? (int)(atof(found+strlen("?value="))+0.5f):0;
      fprintf (stderr, "Found rate call, rate=%d\n", rate);
      set_media_mode(rate ? MEDIA_RESUME:MEDIA_PAUSE);
      if (rate && last_scrub >= 0) {
         set_media_mode_ex(MEDIA_SEEK, NULL, last_scrub, NULL, NULL);
         last_position = last_scrub;
         last_scrub = -1;
      }
   } else if (found = strstr(buffer,"/play"), found) {
      void *found = strstr(buffer, "Content-Location: ");
      if (found) {
         found += strlen("Content-Location: ");
         char *newline = strchr(found, '\n');
         if (newline) *newline = '\0';
         else found = 0;
      }
      if (found && !media_supported(found)) found = 0;
      if (found) {
         fprintf (stderr, "Found content: %s\n", found);
         UrlEncode(found, airplay_url, sizeof airplay_url);
         set_media_mode_url(MEDIA_PLAY, airplay_url);
         if (last_scrub > 0) {
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
      found = strstr(found, "?position=");
      if (found) seek_offset = (int)(atof(found+strlen("?position="))+0.0f);
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
      if (duration) sprintf(content, "duration: %f"NL"position: %f", (double)duration, (double)position);
   } else if (found = strstr(buffer,"/stop"), found) {
      set_media_mode(MEDIA_STOP);
   } else if (found = strstr(buffer,"/photo"), found) {
      fprintf (stderr, "Found photo call retrieve content address\n");
      if (body_length && body) {
         FILE *fp = fopen("/tmp/airplay_photo.jpg", "wb");
         assert(fp);
         int s = fwrite(body, 1, body_length, fp);
         assert(s==body_length);
         fclose(fp);
         set_media_mode_url(MEDIA_PHOTO, "file:///tmp/airplay_photo.jpg");
      }
   } else {
      fprintf (stderr, "Unhandled [%s]\n", buffer);
      status = STATUS_NOT_IMPLEMENTED;
   }
   http_response(filedes, status, http_status_string(status), content[0] ? content:NULL);
   return status==STATUS_OK || status==STATUS_SWITCHING_PROTOCOLS ? 0:-status;
}

int main (int argc, char *argv[])
{
   (void) signal(SIGINT, ex_program);
   //(void) signal ( SIGSEGV ,ex_program);
   int sock;

   if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v') loglevel = 2;

   /* Create the socket and set it up to accept connections. */
   sock = make_socket (PORT);
   if (listen (sock, 1) < 0) {
      perror ("listen");
      exit (EXIT_FAILURE);
   }

   /* Initialize the set of active sockets. */
   FD_ZERO (&active_fd_set);
   FD_SET (sock, &active_fd_set);

   while (1) {
      /* Block until input arrives on one or more active sockets. */
      read_fd_set = active_fd_set;
      if (select (FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0) {
         perror ("select");
         exit (EXIT_FAILURE);
      }
      int socki;
      /* Service all the sockets with input pending. */
      for (socki = 0; socki < FD_SETSIZE; ++socki) {
         if (FD_ISSET (socki, &read_fd_set)) {
            if (socki == sock) {
               /* Connection request on original socket. */
               size_t size = sizeof (clientname);
               int new_sock = accept (sock,
                                      (struct sockaddr *) &clientname,
                                      &size);
               if (new_sock < 0) {
                  perror ("accept");
                  exit (EXIT_FAILURE);
               }
               /*fprintf (stderr,
                        "%i) Server: connect from host %s, port %d.\n",
                        socki, inet_ntoa (clientname.sin_addr),
                        ntohs (clientname.sin_port));*/
               FD_SET (new_sock, &active_fd_set);
            } else {
               /* Data arriving on an already-connected socket. */
               //fprintf (stderr, "%i) Server: connect from existing socket\n", socki);
               if (read_from_client (socki) < 0) {
                  //fprintf (stderr, "%i) Server: connect from existing socket closed\n", socki);
                  close (socki);
                  FD_CLR (socki, &active_fd_set);
               }
            }
         }
      }
   }
}

