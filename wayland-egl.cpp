/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2016 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <dlfcn.h>
#include <pthread.h>

#include <map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "wayland-client.h"
#include "wayland-server.h"
#include "wayland-egl.h"
#include "icegdl-client-protocol.h"
#include "icegdl-server-protocol.h"

#include "gdl.h"
#include "libgma.h"
#include "ismd_core.h"
#include "ismd_vidsink.h"
#include "ismd_vidpproc.h"
#include "ismd_vidrend.h"

#define WAYEGL_UNUSED(x) ((void)x)

#define MIN(x,y) (((x) < (y)) ? (x) : (y))

#define SYSTEM_STRIDE (2048)

#ifndef GMA_API_VERSION
#define GMA_PF_UNDEFINED ((gma_pixel_format_t)-1)
#endif

typedef __eglMustCastToProperFunctionPointerType (*PREALEGLGETPROCADDRESS)( char const * procname );
typedef EGLAPI const char * EGLAPIENTRY (*PREALEGLQUERYSTRING)(EGLDisplay dpy, EGLint name);
typedef EGLAPI EGLImageKHR EGLAPIENTRY (*PREALEGLCREATEIMAGEKHR)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLAPI EGLBoolean EGLAPIENTRY (*PREALEGLDESTROYIMAGEKHR)(EGLDisplay dpy, EGLImageKHR image);
typedef EGLDisplay (*PREALEGLGETDISPLAY)(EGLNativeDisplayType);
typedef EGLSurface (*PREALEGLCREATEWINDOWSURFACE)(EGLDisplay, 
                                                  EGLConfig, 
                                                  EGLNativeWindowType,
                                                  const EGLint *attrib_list);
typedef GDL_API gdl_ret_t (*PREALGDLFLIP)(gdl_plane_id_t plane_id,
                                          gdl_surface_id_t surface_id,
                                          gdl_flip_t sync);

typedef struct _WayEGLDisplay WayEGLDisplay;

struct wl_icegdl_buffer 
{
   struct wl_resource *resource;
   uint32_t format;
   int32_t x;
   int32_t y;
   int32_t width;
   int32_t height;
   int32_t stride;
   uint32_t surfaceId;
};

struct wl_egl_window
{
   WayEGLDisplay *display;
   struct wl_surface *surface;
   struct wl_registry *registry;
   struct wl_icegdl *icegdl;
   struct wl_event_queue *queue;
   bool windowDestroyPending;
   int activeBuffers;
   int width;
   int height;
   int dx;
   int dy;
   int attachedWidth;
   int attachedHeight;
   bool waitForSync;

   EGLNativeWindowType nativeWindow;

   EGLSurface eglSurface;
};

typedef struct _WayEGLDisplay
{
   struct wl_display *display;
   struct wl_global *icegdl;
   struct wl_registry *registry;
   struct wl_event_queue *queue;
   struct wl_egl_window *egl_window;
   struct wl_icegdl *icegdlRemote;
   EGLDisplay eglDisplay;
   EGLNativeWindowType nativeWindow;
} WayEGLDisplay;

typedef struct _wayGdlSurfInfo
{
   bool free;
   gdl_surface_id_t id;
   gma_pixmap_t pixmap;
} wayGdlSurfInfo;


static PREALEGLGETPROCADDRESS gRealEGLGetProcAddress= 0;
static PREALEGLQUERYSTRING gRealEGLQueryString= 0;
static PREALEGLCREATEIMAGEKHR gRealEGLCreateImageKHR= 0;
static PREALEGLDESTROYIMAGEKHR gRealEGLDestroyImageKHR= 0;
static PREALEGLGETDISPLAY gRealEGLGetDisplay= 0;
static PREALEGLCREATEWINDOWSURFACE gRealEGLCreateWindowSurface= 0;
static PREALGDLFLIP gRealGDLFlip= 0;
static pthread_mutex_t gMutex= PTHREAD_MUTEX_INITIALIZER;
static std::vector<struct wl_egl_window*> gNativeWindows= std::vector<struct wl_egl_window*>();
static std::map<void*, WayEGLDisplay*> gDisplayMap= std::map<void*, WayEGLDisplay*>();
static std::map<EGLImageKHR, wayGdlSurfInfo*> gPixmapMap= std::map<EGLImageKHR,wayGdlSurfInfo*>();
static char gExtensions[2048];
static gma_pixmap_funcs_t gmaPixmapFuncs;

static void winRegistryHandleGlobal(void *data,
                                 struct wl_registry *registry, uint32_t id,
		                           const char *interface, uint32_t version);
static void winRegistryHandleGlobalRemove(void *data,
                                       struct wl_registry *registry,
			                              uint32_t name);
static WayEGLDisplay *getDisplay( void *key );
static gma_pixel_format_t getGMAPixelFormatFromGDLPixelFormat( gdl_pixel_format_t gdlFormat );
static wayGdlSurfInfo* createGMAPixmapISMD( uint32_t ismdBufferHandle, int planeNumber );
static gma_pixmap_t createGMAPixmapGDL(wayGdlSurfInfo *wayInfo );
static void destroyGMAPixmapGDL( wayGdlSurfInfo *wayInfo );
static gma_ret_t gmaPixmapDestroyed(gma_pixmap_info_t *pixmapInfo);
static bool configureVideoPlane( WayEGLDisplay *wayEGLDisplay );
static bool initVideoDisplay( WayEGLDisplay *wayEGLDisplay );
static void termVideoDisplay( WayEGLDisplay *wayEGLDisplay );
static void displayVideoFrame( WayEGLDisplay *wayEGLDisplay, struct wl_icegdl_buffer *icegdlBuffer );

static const struct wl_registry_listener winRegistryListener =
{
	winRegistryHandleGlobal,
	winRegistryHandleGlobalRemove
};

struct wl_display* getWLDisplayFromProxy( void *proxy )
{
   struct wl_display *wldisp= 0;

   if ( proxy )
   {
      wldisp= (struct wl_display*)*((void**)(proxy+sizeof(void*)+sizeof(void*)+sizeof(uint32_t)));
   }

   return wldisp;
}

static void* getNativeWindowFromEnv()
{
   void *nw= (void*)0x7; //UPP_C
   const char *env= getenv("WESTEROS_GL_PLANE");
   if ( env )
   {
      nw= (void*)atoi(env);
   }
   printf("wayland-egl: getNativeWindowFronEnv: get plane %p from env\n", nw);
   return nw;
}

static void winIcegdlFormat(void *data, struct wl_icegdl *icegdl, uint32_t format)
{
   WAYEGL_UNUSED(data);
   WAYEGL_UNUSED(icegdl);
   printf("wayland-egl: registry: winIcegdlFormat: %X\n", format);
}

static void winIcegdlPlane(void *data, struct wl_icegdl *icegdl, uint32_t plane )
{
   WAYEGL_UNUSED(icegdl);
   
   struct wl_egl_window *egl_window= (struct wl_egl_window*)data;
   if ( egl_window )
   {
      printf("wayland-egl: winIcegdlPlane: wl_egl_window %p using plane %d\n", egl_window, plane);
      egl_window->nativeWindow= (EGLNativeWindowType)plane;
   }
}

struct wl_icegdl_listener winIcegdlListener = {
	winIcegdlPlane,
	winIcegdlFormat
};

static void winRegistryHandleGlobal(void *data,
                                    struct wl_registry *registry, uint32_t id,
		                              const char *interface, uint32_t version)
{
   struct wl_egl_window *egl_window= (struct wl_egl_window*)data;
   printf("wayland-egl: registry: id %d interface (%s) version %d\n", id, interface, version );
   
   int len= strlen(interface);
   if ( (len==9) && !strncmp(interface, "wl_icegdl", len) ) {
      egl_window->icegdl= (struct wl_icegdl*)wl_registry_bind(registry, id, &wl_icegdl_interface, 1);
      printf("wayland-egl: registry: icegdl %p\n", (void*)egl_window->icegdl);
      wl_proxy_set_queue((struct wl_proxy*)egl_window->icegdl, egl_window->queue);
		wl_icegdl_add_listener(egl_window->icegdl, &winIcegdlListener, egl_window);
		printf("wayland-egl: registry: done add icegdl listener\n");
   }
}

static void winRegistryHandleGlobalRemove(void *data,
                                          struct wl_registry *registry,
			                                 uint32_t name)
{
   WAYEGL_UNUSED(data);
   WAYEGL_UNUSED(registry);
   WAYEGL_UNUSED(name);
}

static void icegdlIBufferDestroy(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

const static struct wl_buffer_interface bufferInterface = {
   icegdlIBufferDestroy
};


static void icegdlDestroyBuffer(struct wl_resource *resource)
{
   struct wl_icegdl_buffer *buffer = (struct wl_icegdl_buffer*)resource->data;

   free(buffer);
}

static void icegdlIGetPlane(struct wl_client *client, struct wl_resource *resource)
{
   if ( client )
   {
      struct wl_display *display= wl_client_get_display( client );
      if ( display )
      {
         WayEGLDisplay *dsp= getDisplay( display );
         if ( dsp )
         {
            if ( !dsp->nativeWindow )
            {
               WayEGLDisplay *disp= getDisplay( dsp->eglDisplay );
               if ( disp )
               {
                  dsp->nativeWindow= disp->nativeWindow;
               }
            }
            if ( !dsp->nativeWindow )
            {
               dsp->nativeWindow= getNativeWindowFromEnv();
            }
            wl_resource_post_event(resource, WL_ICEGDL_PLANE, (unsigned int)dsp->nativeWindow );
         }
      }
   }
}

static void icegdlICreateBuffer(struct wl_client *client, struct wl_resource *resource,
                                uint32_t id, uint32_t gdl_surface_id, int32_t width, int32_t height,
                                uint32_t stride, uint32_t format)
{
   struct wl_icegdl_buffer *buff;
   
   switch (format) 
   {
      case WL_ICEGDL_FORMAT_ARGB8888:
      case WL_ICEGDL_FORMAT_XRGB8888:
      case WL_ICEGDL_FORMAT_YUYV:
      case WL_ICEGDL_FORMAT_RGB565:
         break;
      default:
         wl_resource_post_error(resource, WL_ICEGDL_ERROR_INVALID_FORMAT, "invalid format");
         return;
   }

   buff= (wl_icegdl_buffer*)calloc(1, sizeof *buff);
   if (!buff) 
   {
      wl_resource_post_no_memory(resource);
      return;
   }

   buff->resource= wl_resource_create(client, &wl_buffer_interface, 1, id);
   if (!buff->resource) 
   {
      wl_resource_post_no_memory(resource);
      free(buff);
      return;
   }
   
   buff->width= width;
   buff->height= height;
   buff->format= format;
   buff->stride= stride;
   buff->surfaceId= gdl_surface_id;

   wl_resource_set_implementation(buff->resource,
                                 (void (**)(void)) &bufferInterface,
                                 buff, icegdlDestroyBuffer);

}

static void icegdlICreateBufferPlanar(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id,
                                      uint32_t ismd_buffer_handle,
                                      int32_t x,
                                      int32_t y,
                                      int32_t width,
                                      int32_t height,
                                      uint32_t stride)
{
   struct wl_icegdl_buffer *buff;

   buff= (wl_icegdl_buffer*)calloc(1, sizeof *buff);
   if (!buff) 
   {
      wl_resource_post_no_memory(resource);
      return;
   }

   buff->resource= wl_resource_create(client, &wl_buffer_interface, 1, id);
   if (!buff->resource) 
   {
      wl_resource_post_no_memory(resource);
      free(buff);
      return;
   }
   
   buff->x= x;
   buff->y= y;
   buff->width= width;
   buff->height= height;
   buff->format= WL_ICEGDL_FORMAT_YUV420;
   buff->stride= stride;
   buff->surfaceId= ismd_buffer_handle;

   wl_resource_set_implementation(buff->resource,
                                 (void (**)(void)) &bufferInterface,
                                 buff, icegdlDestroyBuffer);
}                                      

static struct wl_icegdl_interface icegdl_interface = 
{
   icegdlIGetPlane,
   icegdlICreateBuffer,
   icegdlICreateBufferPlanar,
};

static void bind_icegdl(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *resource;
   WayEGLDisplay *wayEGLDisplay= (WayEGLDisplay*)data;

	printf("wayland-egl: bind_icegdl: enter: client %p data %p version %d id %d\n", client, data, version, id);

   resource= wl_resource_create(client, &wl_icegdl_interface, MIN(version, 1), id);
   if (!resource) 
   {
      wl_client_post_no_memory(client);
      return;
   }
   
   if ( !wayEGLDisplay || !wayEGLDisplay->icegdl )
   {
      printf("wayland-egl: bind_icegdl: no valid EGL for compositor\n" );
      wl_client_post_no_memory(client);
      return;
   }

   wl_resource_set_implementation(resource, &icegdl_interface, data, NULL);

   wl_resource_post_event(resource, WL_ICEGDL_FORMAT, WL_ICEGDL_FORMAT_ARGB8888);      
   
   gmaPixmapFuncs.destroy= gmaPixmapDestroyed;
	
	printf("wayland-egl: bind_icegdl: exit: client %p id %d\n", client, id);
}

static WayEGLDisplay *getDisplay( void *key )
{
   WayEGLDisplay *dpy= 0;
   
   pthread_mutex_lock( &gMutex );

   std::map<void*,WayEGLDisplay*>::iterator it= gDisplayMap.find( key );
   if ( it != gDisplayMap.end() )
   {
      dpy= it->second;
   }
   else
   {
      dpy= (WayEGLDisplay*)calloc( 1, sizeof(WayEGLDisplay) );
      if ( dpy )
      {
         dpy->display= (struct wl_display*)key;
         gDisplayMap.insert( std::pair<void*,WayEGLDisplay*>( key, dpy ) );
      }
   }
   pthread_mutex_unlock( &gMutex );
   
   return dpy;
}

EGLBoolean eglBindWaylandDisplayWL( EGLDisplay dpy,
                                    struct wl_display *display )
{
   EGLBoolean result= EGL_FALSE;
   
   WayEGLDisplay *wayEGLDisplay= getDisplay( display );
   
   if ( !wayEGLDisplay->icegdl )
   {
      wayEGLDisplay->icegdl= wl_global_create( display,
                                               &wl_icegdl_interface,
                                               1,
                                               wayEGLDisplay,
                                               bind_icegdl );
      if ( wayEGLDisplay->icegdl )
      {
         WayEGLDisplay *dsp= getDisplay( dpy );
         if ( dsp )
         {
            wayEGLDisplay->eglDisplay= dpy;
            dsp->display= display;
            if ( !wayEGLDisplay->nativeWindow )
            {
               wayEGLDisplay->nativeWindow= dsp->nativeWindow;
               if ( !wayEGLDisplay->nativeWindow )
               {
                  wayEGLDisplay->nativeWindow= getNativeWindowFromEnv();
               }
            }
         }
         result= EGL_TRUE;
      }
   }
   
   return result;
}

EGLBoolean eglUnbindWaylandDisplayWL( EGLDisplay dpy,
                                      struct wl_display *display )
{
   EGLBoolean result= EGL_FALSE;

   WayEGLDisplay *wayEGLDisplay= getDisplay( display );
   
   if ( wayEGLDisplay->icegdl )
   {
      wl_global_destroy( wayEGLDisplay->icegdl );
      wayEGLDisplay->icegdl= NULL;
      result= EGL_TRUE;
   }
   
   return result;
}

EGLBoolean eglQueryWaylandBufferWL( EGLDisplay dpy,
                                    struct wl_resource *resource,
                                    EGLint attribute, EGLint *value )
{
   EGLBoolean result= EGL_FALSE;
   struct wl_icegdl_buffer *icegdlBuffer;
   int icegdlFormat;
   
   if( wl_resource_instance_of( resource, &wl_buffer_interface, &bufferInterface ) ) 
   {
      icegdlBuffer= (wl_icegdl_buffer *)wl_resource_get_user_data( (wl_resource*)resource );
      if ( icegdlBuffer )
      {
         result= EGL_TRUE;
         switch( attribute )
         {
            case EGL_WIDTH:
               *value= icegdlBuffer->width;
               break;
            case EGL_HEIGHT:
               *value= icegdlBuffer->height;
               break;
            case EGL_TEXTURE_FORMAT:
               icegdlFormat= icegdlBuffer->format;
               switch( icegdlFormat )
               {
                  case WL_ICEGDL_FORMAT_ARGB8888:
                  case WL_ICEGDL_FORMAT_XRGB8888:
                     *value= EGL_TEXTURE_RGBA;
                     break;
                  case WL_ICEGDL_FORMAT_YUV420:
                     *value= EGL_TEXTURE_Y_UV_WL;
                     break;
                  default:
                     *value= EGL_NONE;
                     result= EGL_FALSE;
                     break;
               }
               break;
            default:
               result= EGL_FALSE;
               break;
         }
      }
   }
   
   return result;
}

EGLAPI EGLImageKHR EGLAPIENTRY eglCreateImageKHR (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list)
{
   EGLImageKHR image= EGL_NO_IMAGE_KHR;
   
   if ( !gRealEGLCreateImageKHR )
   {
      if ( gRealEGLGetProcAddress )
      {
         gRealEGLCreateImageKHR= (PREALEGLCREATEIMAGEKHR)gRealEGLGetProcAddress( "eglCreateImageKHR" );
      }
      printf("wayland-egl: eglCreateImageKHR: realEGLCreateImageKHR=%p\n", (void*)gRealEGLCreateImageKHR );
      if ( !gRealEGLCreateImageKHR )
      {
         printf("wayland-egl: eglCreateImageKHR: unable to resolve eglCreateImageKHR\n");
         goto exit;
      }
   }

   switch( target )
   {
      case EGL_WAYLAND_BUFFER_WL:
         {
            struct wl_resource *resource;
            struct wl_icegdl_buffer *icegdlBuffer;
            wayGdlSurfInfo *wayInfo= 0;
            gma_pixmap_t pixmap= 0;

            resource= (struct wl_resource *)buffer;
            if( wl_resource_instance_of( resource, &wl_buffer_interface, &bufferInterface ) ) 
            {
               icegdlBuffer= (wl_icegdl_buffer*)wl_resource_get_user_data( resource );
               if ( icegdlBuffer )
               {
                  switch( icegdlBuffer->format )
                  {
                     case WL_ICEGDL_FORMAT_YUV420:
                        {
                           int planeNumber= 0;
                           const EGLint *attrib;
                           
                           attrib= attrib_list;
                           while( attrib )
                           {
                              if ( *attrib == EGL_NONE )
                              {
                                 break;
                              }
                              if ( *attrib == EGL_WAYLAND_PLANE_WL )
                              {
                                 ++attrib;
                                 planeNumber= *attrib;
                                 break;
                              }
                              ++attrib;
                           }
                           switch ( planeNumber )
                           {
                              case 0:
                              case 1:
                                 {
                                    wayInfo= createGMAPixmapISMD( icegdlBuffer->surfaceId, planeNumber );
                                    if ( wayInfo )
                                    {
                                       pixmap= wayInfo->pixmap;
                                    }
                                 }
                                 break;
                              default:
                                 break;
                           }
                        }
                        break;
                     default:
                        {
                           wayInfo= (wayGdlSurfInfo*)malloc( sizeof(wayGdlSurfInfo) );
                           if ( wayInfo )
                           {
                              wayInfo->free= false;
                              wayInfo->id= (gdl_surface_id_t)icegdlBuffer->surfaceId;

                              pixmap= createGMAPixmapGDL( wayInfo );
                           }                           
                        }
                        break;
                  }
                  
                  if ( pixmap )
                  {
                     image= gRealEGLCreateImageKHR( dpy,
                                                    EGL_NO_CONTEXT,
                                                    EGL_NATIVE_PIXMAP_KHR,
                                                    (EGLClientBuffer)pixmap,
                                                    NULL );
                     
                     if ( image )
                     {
                        wayInfo->pixmap= pixmap;
                        pthread_mutex_lock( &gMutex );
                        gPixmapMap.insert( std::pair<EGLImageKHR,wayGdlSurfInfo*>( image, wayInfo ) );
                        pthread_mutex_unlock( &gMutex );
                     }
                  }
               }
            }
         }
         break;
      default:
         image= gRealEGLCreateImageKHR( dpy, ctx, target, buffer, attrib_list );
         break;
   }

exit:
   return image;   
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyImageKHR (EGLDisplay dpy, EGLImageKHR image)
{
   EGLBoolean result= EGL_TRUE;
   
   if ( !gRealEGLDestroyImageKHR )
   {
      if ( gRealEGLGetProcAddress )
      {
         gRealEGLDestroyImageKHR= (PREALEGLDESTROYIMAGEKHR)gRealEGLGetProcAddress( "eglDestroyImageKHR" );
      }
      printf("wayland-egl: eglDestroyImageKHR: realEGLDestroyImageKHR=%p\n", (void*)gRealEGLDestroyImageKHR );
      if ( !gRealEGLDestroyImageKHR )
      {
         printf("wayland-egl: eglCreateImageKHR: unable to resolve eglCreateImageKHR\n");
         goto exit;
      }
   }

   result= gRealEGLDestroyImageKHR( dpy, image );

   {
      pthread_mutex_lock( &gMutex );
      std::map<EGLImageKHR,wayGdlSurfInfo*>::iterator it= gPixmapMap.find( image );
      if ( it != gPixmapMap.end() )
      {
         wayGdlSurfInfo *wayInfo= it->second;
         gma_pixmap_t pixmap= wayInfo->pixmap;
         destroyGMAPixmapGDL( wayInfo );
         gma_pixmap_release( &pixmap );
         gPixmapMap.erase(it);
      }
      pthread_mutex_unlock( &gMutex );
   }
   
exit:
   return result;   
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *procname)
{
   void *proc= 0;
   int len;
   
   if ( !gRealEGLGetProcAddress )
   {
      gRealEGLGetProcAddress= (PREALEGLGETPROCADDRESS)dlsym( RTLD_NEXT, "eglGetProcAddress" );
      printf("wayland-egl: eglGetProcAddress: realEGLGetProcAddress=%p\n", (void*)gRealEGLGetProcAddress );
      if ( !gRealEGLGetProcAddress )
      {
         printf("wayland-egl: eglGetProcAddress: unable to resolve eglGetProcAddress\n");
         goto exit;
      }
   }
   
   len= strlen( procname );
   
   if ( (len == 23) && !strncmp( procname, "eglBindWaylandDisplayWL", len ) )
   {
      proc= (void*)eglBindWaylandDisplayWL;
   }
   else
   if ( (len == 25) && !strncmp( procname, "eglUnbindWaylandDisplayWL", len ) )
   {
      proc= (void*)eglUnbindWaylandDisplayWL;
   }
   else
   if ( (len == 23) && !strncmp( procname, "eglQueryWaylandBufferWL", len ) )
   {
      proc= (void*)eglQueryWaylandBufferWL;
   }
   else
   if ( (len == 17) && !strncmp( procname, "eglCreateImageKHR", len ) )
   {
      proc= (void*)eglCreateImageKHR;
   }
   else
   if ( (len == 18) && !strncmp( procname, "eglDestroyImageKHR", len ) )
   {
      proc= (void*)eglDestroyImageKHR;
   }
   else
   {
      proc= (void*)gRealEGLGetProcAddress( procname );
   }
   
exit:

   return (__eglMustCastToProperFunctionPointerType)proc;
} 

EGLAPI const char * EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
   const char *s= 0;
   
   if ( !gRealEGLQueryString )
   {
      gRealEGLQueryString= (PREALEGLQUERYSTRING)dlsym( RTLD_NEXT, "eglQueryString" );
      printf("wayland-egl: eglQueryString: realEGLQueryString=%p\n", (void*)gRealEGLQueryString );
      if ( !gRealEGLQueryString )
      {
         printf("wayland-egl: eglQueryString: unable to resolve eglQueryString\n");
         goto exit;
      }
   }
   
   s= gRealEGLQueryString( dpy, name );
   
   if ( name == EGL_EXTENSIONS )
   {
      const char *addition= " EGL_WL_bind_wayland_display";
      int len, lenAddition;
      len= s ? strlen( s ) : 0;
      lenAddition= strlen(addition);
      if ( len+lenAddition+1 > sizeof(gExtensions) )
      {
         printf("wayland-egl: eglQueryString: extensions string too large\n");
         goto exit;
      }
      
      gExtensions[0]= '\0';
      if ( s ) strcpy( gExtensions, s );
      strcat( gExtensions, addition );
      
      s= gExtensions;
   }

exit:

   return s;   
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType displayId)
{
   EGLDisplay eglDisplay= 0;

   if ( !gRealEGLGetDisplay )
   {
      gRealEGLGetDisplay= (PREALEGLGETDISPLAY)dlsym( RTLD_NEXT, "eglGetDisplay" );
      printf("wayland-egl: eglGetDisplay: realEGLGetDisplay=%p\n", (void*)gRealEGLGetDisplay );
      if ( !gRealEGLGetDisplay )
      {
         printf("wayland-egl: eglGetDisplay: unable to resolve eglGetDisplay\n");
         goto exit;
      }
   }
   
   eglDisplay= gRealEGLGetDisplay( EGL_DEFAULT_DISPLAY );
   printf("wayland-egl: eglGetDisplay: eglDisplay=%p\n", eglDisplay );
   if ( EGL_NO_DISPLAY == eglDisplay )
   {
      printf("wayland-egl: eglGetDisplay: unable to get display: eglGetError() %X\n", eglGetError());
      goto exit;
   }
   
exit:

   return eglDisplay;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface( EGLDisplay dpy, EGLConfig config,
                                                      EGLNativeWindowType win,
                                                      const EGLint *attrib_list )
{
   EGLSurface eglSurface= 0;
   struct wl_egl_window *egl_window= 0;
   EGLNativeWindowType nativeWindow;
   
   if ( !gRealEGLCreateWindowSurface )
   {
      gRealEGLCreateWindowSurface= (PREALEGLCREATEWINDOWSURFACE)dlsym( RTLD_NEXT, "eglCreateWindowSurface" );
      printf("wayland-egl: eglCreateWindowSurface: realEGLCreateWindowSurface=%p\n", (void*)gRealEGLCreateWindowSurface );
      if ( !gRealEGLCreateWindowSurface )
      {
         printf("wayland-egl: eglCreateWindowSurface: unable to resolve eglCreateWindowSurface\n");
         goto exit;
      }
   }
   
   if ( !win )
   {
      printf("wayland-egl: eglCreateWindowSurface: bad native window\n" );
      goto exit;
   }

   pthread_mutex_lock( &gMutex );
   for( std::vector<struct wl_egl_window*>::iterator it= gNativeWindows.begin();
        it != gNativeWindows.end();
        ++it )
   {
      if ( (*it) == (struct wl_egl_window*)win )
      {
         egl_window= (struct wl_egl_window*)win;
         break;
      }
   }
   pthread_mutex_unlock( &gMutex );
   
   nativeWindow= (egl_window ? egl_window->nativeWindow : win);
   printf("wayland-egl: eglCreateWindowSurface: nativeWindow=%p\n", nativeWindow );
   if ( !nativeWindow )
   {
      nativeWindow= getNativeWindowFromEnv();
   }
   
   eglSurface= gRealEGLCreateWindowSurface( dpy, config, nativeWindow, attrib_list );
   printf("wayland-egl: eglCreateWindowSurface: eglSurface=%p\n", eglSurface );

   if ( egl_window )
   {
      egl_window->eglSurface= eglSurface;
      egl_window->display->nativeWindow= nativeWindow;
   }
   else
   {
      WayEGLDisplay *dsp= getDisplay( dpy );
      if ( dsp )
      {
         dsp->nativeWindow= nativeWindow;
      }
   }
   
exit:   
   return eglSurface;
}

static void buffer_release( void *data, struct wl_buffer *buffer )
{
   wl_egl_window *egl_window= (wl_egl_window*)data;

   --egl_window->activeBuffers;

   wl_buffer_destroy( buffer );

   if ( egl_window->windowDestroyPending && (egl_window->activeBuffers <= 0) )
   {
      wl_egl_window_destroy( egl_window );
   }
}

static struct wl_buffer_listener wl_buffer_listener=
{
   buffer_release
};

GDL_API gdl_ret_t gdl_flip(gdl_plane_id_t   plane_id,
                           gdl_surface_id_t surface_id,
                           gdl_flip_t       sync)
{
   gdl_ret_t ret;
   struct wl_egl_window *egl_window= 0;
   
   if ( !gRealGDLFlip )
   {
      gRealGDLFlip= (PREALGDLFLIP)dlsym( RTLD_NEXT, "gdl_flip" );
      printf("wayland-egl: gdl_flip: realGDLFlip=%p\n", (void*)gRealGDLFlip );
      if ( !gRealGDLFlip )
      {
         printf("wayland-egl: gdl_flip: unable to resolve gdl_flip\n");
         ret= GDL_ERR_FAILED;
         goto exit;
      }
   }

   pthread_mutex_lock( &gMutex );
   for( std::vector<struct wl_egl_window*>::iterator it= gNativeWindows.begin();
        it != gNativeWindows.end();
        ++it )
   {
      struct wl_egl_window *w= (*it);
      if ( w->nativeWindow == (EGLNativeWindowType)plane_id )
      {
         egl_window= (*it);
         break;
      }
   }
   pthread_mutex_unlock( &gMutex );

   if ( egl_window )
   {
      struct wl_buffer *wlBuff= 0;

      if ( egl_window->icegdl && !egl_window->windowDestroyPending  )
      {
         wl_proxy_set_queue((struct wl_proxy*)egl_window->icegdl, egl_window->queue);
         wlBuff= wl_icegdl_create_buffer( egl_window->icegdl, 
                                          (uint32_t)surface_id, 
                                          egl_window->width, 
                                          egl_window->height, 
                                          egl_window->width*4,
                                          WL_ICEGDL_FORMAT_ARGB8888 );
         if ( wlBuff )
         {
            ++egl_window->activeBuffers;
            wl_buffer_add_listener( wlBuff, &wl_buffer_listener, egl_window );

            egl_window->attachedWidth= egl_window->width;
            egl_window->attachedHeight= egl_window->height;
            wl_surface_attach( egl_window->surface, wlBuff, egl_window->dx, egl_window->dy );
            wl_surface_damage( egl_window->surface, 0, 0, egl_window->width, egl_window->height);
            wl_surface_commit( egl_window->surface );   

            wl_display_flush( egl_window->display->display );
         }

         wl_display_dispatch_queue_pending( egl_window->display->display, egl_window->queue );

         if ( egl_window->waitForSync )
         {
            gdl_display_wait_for_vblank( GDL_DISPLAY_ID_0, NULL );
         }
         egl_window->waitForSync= !egl_window->waitForSync;
      }
   }
   else
   {
      gRealGDLFlip( plane_id, surface_id, sync );
   }

   ret= GDL_SUCCESS;
   
exit:
   return ret;
}

struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height)
{
   struct wl_egl_window *egl_window= 0;
   struct wl_display *wldisp;

   wldisp= getWLDisplayFromProxy(surface);
   if ( wldisp )
   {
      WayEGLDisplay *display= getDisplay( wldisp );
      if ( display )
      {
         egl_window= (wl_egl_window*)calloc( 1, sizeof(struct wl_egl_window) );
         if ( !egl_window )
         {
            printf("wayland-egl: wl_egl_window_create: failed to allocate wl_egl_window\n");
            goto exit;
         }

         egl_window->display= display;
         egl_window->surface= surface;
         egl_window->width= width;
         egl_window->height= height;
         egl_window->windowDestroyPending= false;
         egl_window->activeBuffers= 0;

         egl_window->queue= wl_display_create_queue(egl_window->display->display);
         if ( !egl_window->queue )
         {
            printf("wayland-egl: wl_egl_window_create: unable to create event queue\n");
            free( egl_window );
            egl_window= 0;
            goto exit;
         }

         egl_window->registry= wl_display_get_registry( egl_window->display->display );
         if ( !egl_window->registry )
         {
            printf("wayland-egl: wl_egl_window_create: unable to get display registry\n");
            free( egl_window );
            egl_window= 0;
            goto exit;
         }
         wl_proxy_set_queue((struct wl_proxy*)egl_window->registry, egl_window->queue);
         wl_registry_add_listener(egl_window->registry, &winRegistryListener, egl_window);
         wl_display_roundtrip_queue(egl_window->display->display, egl_window->queue);

         if ( !egl_window->icegdl )
         {
            printf("wayland-egl: wl_egl_window_create: no wl_icegdl protocol available\n");
            wl_egl_window_destroy( egl_window );
            egl_window= 0;
            goto exit;
         }

         wl_icegdl_get_plane( egl_window->icegdl );
         wl_display_roundtrip_queue(egl_window->display->display, egl_window->queue);

         pthread_mutex_lock( &gMutex );
         gNativeWindows.push_back( egl_window );
         pthread_mutex_unlock( &gMutex );
      }
   }

exit:
   
   return egl_window;
}

void wl_egl_window_destroy(struct wl_egl_window *egl_window)
{
   if ( egl_window )
   {
      pthread_mutex_lock( &gMutex );
      for( std::vector<struct wl_egl_window*>::iterator it= gNativeWindows.begin();
           it != gNativeWindows.end();
           ++it )
      {
         if ( (*it) == egl_window )
         {
            gNativeWindows.erase(it);
            break;
         }
      }
      pthread_mutex_unlock( &gMutex );

      if ( egl_window->activeBuffers )
      {
         egl_window->windowDestroyPending= true;
      }
      else
      {
         pthread_mutex_lock( &gMutex );
         for( std::vector<struct wl_egl_window*>::iterator it= gNativeWindows.begin();
              it != gNativeWindows.end();
              ++it )
         {
            if ( (*it) == egl_window )
            {
               gNativeWindows.erase(it);
               break;
            }
         }
         pthread_mutex_unlock( &gMutex );

         if ( egl_window->icegdl )
         {
            wl_proxy_set_queue((struct wl_proxy*)egl_window->icegdl, 0);
            wl_icegdl_destroy( egl_window->icegdl );
            egl_window->icegdl= 0;
         }

         if ( egl_window->registry )
         {
            wl_proxy_set_queue((struct wl_proxy*)egl_window->registry, 0);
            wl_registry_destroy(egl_window->registry);
            egl_window->registry= 0;
         }
         
         if ( egl_window->queue )
         {
            wl_event_queue_destroy( egl_window->queue );
            egl_window->queue= 0;
         }

         free( egl_window );
      }
   }
}

void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height, int dx, int dy)
{
   if ( egl_window )
   {
      egl_window->dx += dx;
      egl_window->dy += dy;
      egl_window->width= width;
      egl_window->height= height;
   }
}

void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width, int *height)
{
   if ( egl_window )
   {
      if ( width )
      {
         *width= egl_window->attachedWidth;
      }
      if ( height )
      {
         *height= egl_window->attachedHeight;
      }
   }
}

static void wayIcegdlFormat(void *data, struct wl_icegdl *icegdl, uint32_t format)
{
   WAYEGL_UNUSED(data);
   WAYEGL_UNUSED(icegdl);
   printf("wayland-egl: registry: wayIcegdlFormat: %X\n", format);
}

static void wayIcegdlPlane(void *data, struct wl_icegdl *icegdl, uint32_t plane )
{
   WAYEGL_UNUSED(icegdl);

   WayEGLDisplay *dsp= (WayEGLDisplay*)data;
   if ( dsp )
   {
      printf("wayland-egl: wayIcegdlPlane: dsp %p using plane %d\n", dsp, plane);
      dsp->nativeWindow= (EGLNativeWindowType)plane;
   }
}

struct wl_icegdl_listener wayIcegdlListener = {
	wayIcegdlPlane,
	wayIcegdlFormat
};


static void wayRegistryHandleGlobal(void *data,
                                    struct wl_registry *registry, uint32_t id,
		                              const char *interface, uint32_t version)
{
   WayEGLDisplay *disp= (WayEGLDisplay*)data;

   int len= strlen(interface);
   if ( (len==9) && !strncmp(interface, "wl_icegdl", len) ) {
      disp->icegdlRemote= (struct wl_icegdl*)wl_registry_bind(registry, id, &wl_icegdl_interface, 1);
      printf("wayland-egl: registry: icegdl %p\n", (void*)disp->icegdlRemote);
      wl_proxy_set_queue((struct wl_proxy*)disp->icegdlRemote, disp->queue);
		wl_icegdl_add_listener(disp->icegdlRemote, &wayIcegdlListener, disp);
		printf("wayland-egl: registry: done add icegdl listener\n");
   }
}

static void wayRegistryHandleGlobalRemove(void *data,
                                          struct wl_registry *registry,
			                                 uint32_t name)
{
   WAYEGL_UNUSED(data);
   WAYEGL_UNUSED(registry);
   WAYEGL_UNUSED(name);
}

static const struct wl_registry_listener wayRegistryListener =
{
	wayRegistryHandleGlobal,
	wayRegistryHandleGlobalRemove
};

extern "C" {

bool wl_egl_remote_begin( struct wl_display *dspsrc, struct wl_display *dspdest )
{
   bool result= false;
   WayEGLDisplay *display;
   struct wl_egl_window *egl_window= 0;

   display= getDisplay( dspdest );
   if ( display )
   {
      if ( !display->icegdlRemote )
      {
         display->registry= wl_display_get_registry( dspdest );
         if ( display->registry )
         {
            display->queue= wl_display_create_queue(dspdest);
            if ( display->queue )
            {
               wl_proxy_set_queue((struct wl_proxy*)display->registry, display->queue);
               wl_registry_add_listener(display->registry, &wayRegistryListener, display);
               wl_display_roundtrip_queue(dspdest, display->queue);

               if ( display->icegdlRemote )
               {
                  wl_icegdl_get_plane( display->icegdlRemote );
                  wl_display_roundtrip_queue(dspdest, display->queue);
                  if ( display->nativeWindow != 0 )
                  {
                     WayEGLDisplay *dispSrc;
                     dispSrc= getDisplay( dspsrc );
                     if ( dispSrc )
                     {
                        dispSrc->nativeWindow= display->nativeWindow;
                        wl_proxy_set_queue((struct wl_proxy*)display->icegdlRemote, 0);
                        result= true;
                     }
                  }
               }
            }
         }
      }
   }

   return result;
}

void wl_egl_remote_end( struct wl_display *dspsrc, struct wl_display *dspdest )
{
   WAYEGL_UNUSED(dspsrc);

   pthread_mutex_lock( &gMutex );

   std::map<void*,WayEGLDisplay*>::iterator it= gDisplayMap.find( dspdest );
   if ( it != gDisplayMap.end() )
   {
      WayEGLDisplay *display= it->second;
      if ( display )
      {
         if ( display->icegdlRemote )
         {
            wl_icegdl_destroy( display->icegdlRemote );
            display->icegdlRemote= 0;
         }
         if ( display->registry )
         {
            wl_registry_destroy(display->registry);
            display->registry= 0;
         }
         if ( display->queue )
         {
            wl_event_queue_destroy( display->queue );
            display->queue= 0;
         }
         free( display );
      }
      gDisplayMap.erase( it );
   }

   pthread_mutex_unlock( &gMutex );
}

struct wl_buffer* wl_egl_remote_buffer_clone( struct wl_display *dspsrc,
                                              struct wl_resource *resource,
                                              struct wl_display *dspdest,
                                              int *width,
                                              int *height )
{
   struct wl_buffer *clone= 0;

   if( wl_resource_instance_of( resource, &wl_buffer_interface, &bufferInterface ) )
   {
      struct wl_icegdl_buffer *icegdlBuffer;

      icegdlBuffer= (wl_icegdl_buffer *)wl_resource_get_user_data( (wl_resource*)resource );
      if ( icegdlBuffer )
      {
         WayEGLDisplay *display;

         display= getDisplay( dspdest );
         if ( display )
         {
            if ( display->icegdlRemote )
            {
               switch ( icegdlBuffer->format )
               {
                  case WL_ICEGDL_FORMAT_ARGB8888:
                  case WL_ICEGDL_FORMAT_XRGB8888:
                  case WL_ICEGDL_FORMAT_YUYV:
                  case WL_ICEGDL_FORMAT_RGB565:
                     clone= wl_icegdl_create_buffer( display->icegdlRemote,
                                                     icegdlBuffer->surfaceId,
                                                     icegdlBuffer->width,
                                                     icegdlBuffer->height,
                                                     icegdlBuffer->stride,
                                                     icegdlBuffer->format );
                     break;
                  case WL_ICEGDL_FORMAT_YUV420:
                     clone= wl_icegdl_create_planar_buffer( display->icegdlRemote,
                                                            icegdlBuffer->surfaceId,
                                                            icegdlBuffer->x,
                                                            icegdlBuffer->y,
                                                            icegdlBuffer->width,
                                                            icegdlBuffer->height,
                                                            icegdlBuffer->stride );
                     break;
               }
               if ( clone )
               {
                  if ( width ) *width= icegdlBuffer->width;
                  if ( height ) *height= icegdlBuffer->height;
               }
            }
         }
      }
   }

   return clone;
}

}

static gma_pixel_format_t getGMAPixelFormatFromGDLPixelFormat( gdl_pixel_format_t gdlFormat )
{
   gma_pixel_format_t gmaFormat;
   
   switch( gdlFormat )
   {
      case GDL_PF_ARGB_32:
         gmaFormat= GMA_PF_ARGB_32;
         break;
      case GDL_PF_RGB_32:
         gmaFormat= GMA_PF_RGB_32;
         break;
      case GDL_PF_ARGB_16_1555:
         gmaFormat= GMA_PF_ARGB_16_1555;
         break;
      case GDL_PF_ARGB_16_4444:
         gmaFormat= GMA_PF_ARGB_16_4444;
         break;
      case GDL_PF_RGB_16:
         gmaFormat= GMA_PF_RGB_16;
         break;
      case GDL_PF_A8:
         gmaFormat= GMA_PF_A8;
         break;
      case GDL_PF_AY16:
         gmaFormat= GMA_PF_AY16;
         break;
      default:
         gmaFormat= GMA_PF_UNDEFINED;
         break;
   }
   
   return gmaFormat;
}

static wayGdlSurfInfo* createGMAPixmapISMD( uint32_t ismdBufferHandle, int planeNumber )
{
   ismd_result_t ismdrc;
   ismd_buffer_descriptor_t desc;
   ismd_frame_attributes_t *attr= 0;
   wayGdlSurfInfo *wayInfo= 0;
   gdl_surface_info_t surfaceInfo;
   gma_pixmap_t pixmap= 0;
   int stride;
   gdl_ret_t gdlrc;

   ismdrc= ismd_buffer_read_desc(ismdBufferHandle, &desc);
   if (ismdrc == ISMD_SUCCESS) 
   {
      attr= (ismd_frame_attributes_t *)desc.attributes;

      stride= attr->scanline_stride;
      if ( stride == 0 )
      {
         stride= SYSTEM_STRIDE;
      }
      
      wayInfo= (wayGdlSurfInfo*)malloc( sizeof(wayGdlSurfInfo) );
      if ( wayInfo )
      {
         switch( planeNumber )
         {
            case 0:
               // Y surface
               surfaceInfo.flags= 0;
               surfaceInfo.pixel_format= GDL_PF_A8;
               surfaceInfo.width= attr->cont_size.width;
               surfaceInfo.height= attr->cont_size.height;
               surfaceInfo.pitch= stride;
               surfaceInfo.size= surfaceInfo.height*stride;
               surfaceInfo.phys_addr= desc.phys.base + attr->y;
               surfaceInfo.y_size= 0;
               surfaceInfo.u_offset= 0;
               surfaceInfo.v_offset= 0;
               surfaceInfo.uv_size= 0;
               surfaceInfo.uv_pitch= 0;
               
               gdlrc= gdl_create_surface( &surfaceInfo );
               if ( gdlrc == GDL_SUCCESS )
               {
                  wayInfo->free= true;
                  wayInfo->id= surfaceInfo.id;
                  
                  pixmap= createGMAPixmapGDL( wayInfo );          
               }
               break;
            case 1:
               // UV surface
               surfaceInfo.flags= 0;
               surfaceInfo.pixel_format= GDL_PF_AY16;
               surfaceInfo.width= attr->cont_size.width/2;
               if ( attr->pixel_format == ISMD_PF_NV12 )
               {
                  surfaceInfo.height= attr->cont_size.height/2;
               }
               else
               {
                  surfaceInfo.height= attr->cont_size.height;
               }
               surfaceInfo.pitch= stride;
               surfaceInfo.size= surfaceInfo.height*stride;
               surfaceInfo.phys_addr= desc.phys.base + attr->u;
               surfaceInfo.y_size= 0;
               surfaceInfo.u_offset= 0;
               surfaceInfo.v_offset= 0;
               surfaceInfo.uv_size= 0;
               surfaceInfo.uv_pitch= 0;
               
               gdlrc= gdl_create_surface( &surfaceInfo );
               if ( gdlrc == GDL_SUCCESS )
               {
                  wayInfo->free= true;
                  wayInfo->id= surfaceInfo.id;
                  
                  pixmap= createGMAPixmapGDL( wayInfo );          
               }
               break;
         }
      }
   }
   
   if ( pixmap )
   {
      wayInfo->pixmap= pixmap;
   }
   else
   {
      if ( wayInfo )
      {
         free( wayInfo );
         wayInfo= 0;
      }
   }
   
   return wayInfo;
}

static gma_pixmap_t createGMAPixmapGDL(wayGdlSurfInfo *wayInfo )
{
   gma_pixmap_t pixmap= 0;
   gdl_ret_t retGDL;
   gma_ret_t retGMA;
   gdl_surface_id_t surfaceId;
   gdl_surface_info_t surfaceInfo;
   gdl_uint8 *virt;
   gma_pixmap_info_t pixmapInfo;
   
   surfaceId= wayInfo->id;
   
   retGDL= gdl_get_surface_info( surfaceId, &surfaceInfo );
   if ( retGDL == GDL_SUCCESS )
   {
      retGDL= gdl_map_surface(surfaceId, &virt, NULL );
      if ( retGDL == GDL_SUCCESS )
      {
         pixmapInfo.type= GMA_PIXMAP_TYPE_PHYSICAL;
         pixmapInfo.virt_addr= virt;
         pixmapInfo.phys_addr= surfaceInfo.phys_addr;
         pixmapInfo.width= surfaceInfo.width;
         pixmapInfo.height= surfaceInfo.height;
         pixmapInfo.pitch= surfaceInfo.pitch;
         pixmapInfo.format= getGMAPixelFormatFromGDLPixelFormat( surfaceInfo.pixel_format );
         pixmapInfo.user_data= (void*)wayInfo;
         
         retGMA= gma_pixmap_alloc( &pixmapInfo, 
                                   &gmaPixmapFuncs,
                                   &pixmap );
         if ( retGMA != GMA_SUCCESS )
         {
            printf( "wayland-egl: createGMAPixmapGDL: gma_pixmap_alloc error: %d\n", retGMA );
            pixmap= 0;
         }
      }
      else
      {
         printf( "wayland-egl: createGMAPixmapGDL: gdl_map_surface error: %d\n", retGDL );
      }
   }
   else
   {
      printf( "wayland-egl: createGMAPixmapGDL: gdl_get_surface_info error: %d\n", retGDL );
   }

   return pixmap;
}

static void destroyGMAPixmapGDL( wayGdlSurfInfo *wayInfo )
{
   gdl_ret_t retGDL;
   if ( wayInfo )
   {
      gdl_surface_info_t surfaceInfo;
      retGDL= gdl_get_surface_info( wayInfo->id, &surfaceInfo );
      if ( retGDL == GDL_SUCCESS )
      {
         retGDL= gdl_unmap_surface( wayInfo->id );
         if ( retGDL != GDL_SUCCESS )
         {
            printf( "wayland-egl: destroyGMAPixmapGDL: gdl_unmap_surface error: %d\n", retGDL );
         }
      }
   }
}

static gma_ret_t gmaPixmapDestroyed(gma_pixmap_info_t *pixmapInfo)
{
   wayGdlSurfInfo *wayInfo;

   wayInfo= (wayGdlSurfInfo*)pixmapInfo->user_data;
   if ( wayInfo )
   {
      if ( wayInfo->free )
      {
         gdl_free_surface( wayInfo->id );
      }
      
      free( wayInfo );
   }
   return GMA_SUCCESS;
}

