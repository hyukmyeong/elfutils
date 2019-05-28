#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>
#include <curl/curl.h>

const char *dbgserver_envvar = "DEBUGINFO_SERVER";
const char *tmp_filename = "dbgserver_anon";

int
dbgserver_enabled (void)
{
  return getenv(dbgserver_envvar) != NULL;
}

size_t
write_callback (char *ptr, size_t size, size_t nmemb, void *fdptr)
{
  int fd = *(int*)fdptr;
  ssize_t res;
  ssize_t count = size * nmemb;

  res = write(fd, (void*)ptr, count);

  if (res < 0)
    return (size_t)0;

  return (size_t)res;
}

/* TODO: handle errors with DWARF_E_*.  */
int
dbgserver_find_debuginfo (const unsigned char *build_id, int build_id_len)
{
  int fd;
  int url_len;
  long respcode;
  char *url;
  char *url_base;
  char idbuf[65];
  CURL *session;
  CURLcode curl_res;

  url_base = getenv(dbgserver_envvar);
  if (url_base == NULL
      || curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
    return -1;

  session = curl_easy_init();
  if (session == NULL)
    {
      curl_global_cleanup();
      return -1;
    }

  /* copy hex representation of buildid into idbuf.  */
  for (int i = 0; i < build_id_len; i++)
    sprintf(idbuf + (i * 2), "%02x", build_id[i]);

  /* url format: $DEBUGINFO_SERVER/buildid/HEXCODE/HEXCODE.debug  */
  url_len = strlen(url_base) + strlen("/buildid/")
            + build_id_len * 4 + strlen("/.debug") + 1;

  url = (char*)malloc(url_len);
  if (url == NULL)
      return -1;

  sprintf(url, "%s/buildid/%s/%s.debug", url_base, idbuf, idbuf);

  fd = syscall(__NR_memfd_create, tmp_filename, 0);
  curl_easy_setopt(session, CURLOPT_URL, url);
  curl_easy_setopt(session, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(session, CURLOPT_WRITEDATA, (void*)&fd);

  curl_res = curl_easy_perform(session);
  curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &respcode);
  curl_easy_cleanup(session);
  curl_global_cleanup();
  free(url);

  if (curl_res == CURLE_OK && respcode == 200)
    return fd;

  close(fd);
  return -1;
}
