#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/system/tagmanager/artsearch.h"
#include "src/system/net/http.h"

static int mode;
static char last_url[2048];
static const unsigned char jpeg[]={0xff,0xd8,0xff,0xc0,0,7,8,0,32,0,48,0xff,0xd9};
bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout, int *status) {
    (void)timeout; (void)status; assert(req && req->no_https_downgrade); snprintf(last_url,sizeof(last_url),"%s",url);
    if(mode==3) return false;
    const char *json="{\"results\":[{\"collectionName\":\"Album\",\"artistName\":\"Artist\",\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/a/100x100bb.jpg\"},{\"collectionName\":\"Malicious\",\"artworkUrl100\":\"https://mzstatic.com.evil.example/p.jpg\"},{\"collectionName\":\"Plain\",\"artworkUrl100\":\"http://is1.mzstatic.com/a.jpg\"}]}";
    size_t n=mode==1?sizeof(jpeg):mode==2?limit:strlen(json);
    *out=malloc(n+1); assert(*out);
    if(mode==1) memcpy(*out,jpeg,n); else if(mode==2) memset(*out,'X',n); else memcpy(*out,json,n);
    (*out)[n]=0; if(out_len) *out_len=n; return true;
}
char *http_url_encode(const char *in, char *out, size_t size) {
    (void)in; snprintf(out,size,"Artist%%20Album"); return out;
}
int main(void) {
    artsearch_result_t list[ARTSEARCH_MAX_RESULTS]; int count=0;
    assert(artsearch_find("Artist Album",list,&count) && count==1);
    assert(strstr(last_url,"Artist%20Album") && strstr(list[0].url,"600x600bb.jpg"));
    assert(!strcmp(list[0].artist,"Artist") && !strcmp(list[0].album,"Album"));
    uint8_t *data=NULL; size_t n=0; int w=0,h=0; mode=1;
    assert(artsearch_download(list[0].url,&data,&n,&w,&h));
    assert(n==sizeof(jpeg) && w==48 && h==32); free(data);
    assert(!artsearch_download("https://mzstatic.com.evil.example/a.jpg",&data,&n,&w,&h));
    assert(!artsearch_download("https://user@is1.mzstatic.com/a.jpg",&data,&n,&w,&h));
    mode=2; assert(!artsearch_download(list[0].url,&data,&n,&w,&h));
    mode=3; assert(!artsearch_find("Artist Album",list,&count));
    puts("art search tests passed");
    return 0;
}
