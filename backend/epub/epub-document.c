#include "ev-file-helpers.h"
#include "epub-document.h"
#include "unzip.h"
#include "ev-document-thumbnails.h"
#include "ev-document-misc.h"
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include <config.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <webkit/webkit.h>
#include <gtk/gtk.h>

typedef enum _xmlParseReturnType 
{
    XML_ATTRIBUTE,
    XML_KEYWORD
}xmlParseReturnType;

typedef struct _contentListNode {  
    gchar* key ;
    gchar* value ;
	gint index ;
}contentListNode;

typedef struct _EpubDocumentClass EpubDocumentClass;

struct _EpubDocumentClass
{
    EvDocumentClass parent_class;
};

struct _EpubDocument
{
    EvDocument parent_instance;
	/*Stores the path to the source archive*/
    gchar* archivename ;
	/*Stores the path of the directory where we unzipped the epub*/
    gchar* tmp_archive_dir ;
	/*Stores the contentlist in a sorted manner*/
    GList* contentList ;
	/*uri of the current page being displayed in the webview*/
    gchar* currentpageuri ;
    /* A variable to hold our epubDocument for unzipping*/
    unzFile epubDocument ;
};

static void       epub_document_document_thumbnails_iface_init (EvDocumentThumbnailsInterface *iface);

EV_BACKEND_REGISTER_WITH_CODE (EpubDocument, epub_document,
	{
		EV_BACKEND_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_THUMBNAILS,
						epub_document_document_thumbnails_iface_init);
	} );

static void
epub_webkit_render(cairo_surface_t **surface,
                   const char* uri);
static GdkPixbuf *
epub_document_thumbnails_get_thumbnail (EvDocumentThumbnails *document,
					  EvRenderContext      *rc,
					  gboolean              border)
{
	GdkPixbuf *thumbnail;
	cairo_surface_t* surface=NULL;
	gchar* uri = (gchar*) rc->page->backend_page;
	epub_webkit_render (&surface,uri);
	while ( !surface ) 
		;
	gint width = cairo_image_surface_get_width (surface);
	gint height = cairo_image_surface_get_height (surface);
	if (surface) {
		thumbnail = ev_document_misc_pixbuf_from_surface (surface);
		cairo_surface_destroy (surface);
	}
	return thumbnail;
}

static void
epub_document_thumbnails_get_dimensions (EvDocumentThumbnails *document,
					   EvRenderContext      *rc,
					   gint                 *width,
					   gint                 *height)
{
	gdouble page_width = 800;
	gdouble page_height = 600;
	*width = MAX ((gint)(page_width * rc->scale + 0.5), 1);
	*height = MAX ((gint)(page_height * rc->scale + 0.5), 1);
}

static void
epub_document_document_thumbnails_iface_init (EvDocumentThumbnailsInterface *iface)
{
	iface->get_thumbnail = epub_document_thumbnails_get_thumbnail;
	iface->get_dimensions = epub_document_thumbnails_get_dimensions;
}

static gboolean
epub_document_save (EvDocument *document,
		      const char *uri,
		      GError    **error)
{
	EpubDocument *epub_document = EPUB_DOCUMENT (document);

	return ev_xfer_uri_simple (epub_document->archivename, uri, error);
}

static int
epub_document_get_n_pages (EvDocument *document)
{
	EpubDocument *epub_document = EPUB_DOCUMENT (document);

        if (epub_document-> contentList == NULL)
                return 0;
            
	return g_list_length(epub_document->contentList);
}

static void 
webkit_render_cb(GtkWidget *web_view,
                 GParamSpec *specification,
           	     cairo_surface_t **surface)
{
	WebKitLoadStatus status = webkit_web_view_get_load_status (WEBKIT_WEB_VIEW(web_view));

	if ( status == WEBKIT_LOAD_FINISHED )
	{
		*(surface) = webkit_web_view_get_snapshot (WEBKIT_WEB_VIEW(web_view));
	}
}

static void
epub_webkit_render(cairo_surface_t **surface,
                   const char* uri)
{
	GtkWidget *offscreen_window = gtk_offscreen_window_new ();
	gtk_window_set_default_size(GTK_WINDOW(offscreen_window),800,600);
	GtkWidget* scroll_view = gtk_scrolled_window_new (NULL,NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scroll_view),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
	GtkWidget* web_view = webkit_web_view_new ();
	g_signal_connect(WEBKIT_WEB_VIEW(web_view),"notify::load-status",G_CALLBACK(webkit_render_cb),surface);
	gtk_container_add(GTK_CONTAINER(scroll_view),web_view);
	gtk_container_add(GTK_CONTAINER(offscreen_window),scroll_view);
	gtk_widget_show_all (offscreen_window);
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web_view),uri);
}

/**
 * epub_remove_temporary_dir : Removes a directory recursively. 
 * This function is same as comics_remove_temporary_dir
 * Returns:
 *   	0 if it was successfully deleted,
 * 	-1 if an error occurred 		
 */
static int 
epub_remove_temporary_dir (gchar *path_name) 
{
	GDir  *content_dir;
	const gchar *filename;
	gchar *filename_with_path;
	
	if (g_file_test (path_name, G_FILE_TEST_IS_DIR)) {
		content_dir = g_dir_open  (path_name, 0, NULL);
		filename  = g_dir_read_name (content_dir);
		while (filename) {
			filename_with_path = 
				g_build_filename (path_name, 
						  filename, NULL);
			epub_remove_temporary_dir (filename_with_path);
			g_free (filename_with_path);
			filename = g_dir_read_name (content_dir);
		}
		g_dir_close (content_dir);
	}
	/* Note from g_remove() documentation: on Windows, it is in general not 
	 * possible to remove a file that is open to some process, or mapped 
	 * into memory.*/
	return (g_remove (path_name));
}


static gboolean
check_mime_type             (const gchar* uri,
                             GError** error);

static gboolean 
open_xml_document           (const gchar* filename);

static gboolean 
set_xml_root_node           (xmlChar* rootname);

static xmlNodePtr
xml_get_pointer_to_node     (xmlChar* parserfor,
                             xmlChar* attributename,
                             xmlChar* attributevalue);
static void 
xml_parse_children_of_node  (xmlNodePtr parent, 
                             xmlChar* parserfor,
                             xmlChar* attributename,
                             xmlChar* attributevalue);

static gboolean 
xml_check_attribute_value   (xmlNode* node,
                             xmlChar * attributename,
                             xmlChar* attributevalue);

static xmlChar* 
xml_get_data_from_node      (xmlNodePtr node,
                             xmlParseReturnType rettype,
                             xmlChar* attributename);

static void 
xml_free_doc();

static void
free_tree_nodes             (gpointer data);

/*Global variables for XML parsing*/
static xmlDocPtr    xmldocument ;
static xmlNodePtr   xmlroot ;
static xmlNodePtr   xmlretval ;

/*
**Functions to parse the xml files.
**Open a XML document for reading 
*/
static gboolean 
open_xml_document ( const gchar* filename )
{
	xmldocument = xmlParseFile(filename);

	if ( xmldocument == NULL )
	{
		return FALSE ;
	}
	else
	{
		return TRUE ;
	}
}

/**
 *Check if the root value is same as rootname .
 *if supplied rootvalue = NULL ,just set root to rootnode . 
**/
static gboolean 
set_xml_root_node(xmlChar* rootname)
{
	xmlroot = xmlDocGetRootElement(xmldocument);
	
	if (xmlroot == NULL) {

		xmlFreeDoc(xmldocument);	
		return FALSE;
	}

    if ( rootname == NULL )
    {
        return TRUE ;
    }

    if ( !xmlStrcmp(xmlroot->name,rootname))
    {
        return TRUE ;
    }
    else
    {
	   return FALSE;
    }
} 

static xmlNodePtr
xml_get_pointer_to_node(xmlChar* parserfor,
                        xmlChar*  attributename,
                        xmlChar* attributevalue )
{
    xmlNodePtr topchild,children ;

    xmlretval = NULL ;

    if ( !xmlStrcmp( xmlroot->name, parserfor) )
    {
        return xmlroot ;
    }

    topchild = xmlroot->xmlChildrenNode ;

    while ( topchild != NULL )
    {
        if ( !xmlStrcmp(topchild->name,parserfor) )
        {
            if ( xml_check_attribute_value(topchild,attributename,attributevalue) == TRUE )
            {
                 xmlretval = topchild;
                 return xmlretval;     
            }
            else 
            {
                /*No need to parse children node*/
                topchild = topchild->next ;
                continue ;
            }
        }

        xml_parse_children_of_node(topchild , parserfor, attributename, attributevalue) ;

        topchild = topchild->next ;
    }

    return xmlretval ;
}

static void 
xml_parse_children_of_node(xmlNodePtr parent, 
                           xmlChar* parserfor,
                           xmlChar* attributename,
                           xmlChar* attributevalue )
{
    xmlNodePtr child = parent->xmlChildrenNode ;
    
    while ( child != NULL )
    {
        if ( !xmlStrcmp(child->name,parserfor))
        {
            if ( xml_check_attribute_value(child,attributename,attributevalue) == TRUE )
            {
                 xmlretval = child;
                 return ;
            }
            else 
            {
                /*No need to parse children node*/
                child = child->next ;
                continue ;
            }
        }

        /*return already if we have xmlretval set*/
        if ( xmlretval != NULL )
        {
            return ;
        }

        xml_parse_children_of_node(child,parserfor,attributename,attributevalue) ;
        child = child->next ;
    }
}

static void 
xml_free_doc()
{
    xmlFreeDoc(xmldocument);
}

static gboolean 
xml_check_attribute_value(xmlNode* node,
                          xmlChar * attributename,
                          xmlChar* attributevalue)
{
    xmlChar* attributefromfile ;
    if ( attributename == NULL || attributevalue == NULL )
    {
         return TRUE ;     
    }
    else if ( !xmlStrcmp(( attributefromfile = xmlGetProp(node,attributename)),
                           attributevalue) )
    {
        xmlFree(attributefromfile);
        return TRUE ;
    }
    xmlFree(attributefromfile);
    return FALSE ;
}

static xmlChar* 
xml_get_data_from_node(xmlNodePtr node,
                       xmlParseReturnType rettype,
                       xmlChar* attributename)
{
    xmlChar* datastring ;
    if ( rettype == XML_ATTRIBUTE )
       datastring= xmlGetProp(node,attributename);
    else
       datastring= xmlNodeListGetString(xmldocument,node->xmlChildrenNode, 1);

    return datastring;
}

static gboolean
check_mime_type(const gchar* uri,GError** error)
{
    GError * err = NULL ;
    gchar* mimeFromFile = ev_file_get_mime_type(uri,FALSE,&err);
    
    if ( !mimeFromFile )
    {
        if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("Unknown MIME Type"));
        }
        return FALSE;
    }
    else if ( g_strcmp0(mimeFromFile, "application/epub+zip") == 0  )
    {
        return TRUE ;
    }
    else
    {
        g_set_error_literal (error,
                     EV_DOCUMENT_ERROR,
                     EV_DOCUMENT_ERROR_INVALID,
                     _("Not an ePub document"));

        return FALSE;
    }
}

static gboolean
extract_one_file(EpubDocument* epub_document,GError ** error)
{
    GFile * outfile ;
    gsize writesize = 0;
    GString * gfilepath ;
    unz_file_info64 info ;  
    gchar* directory;
    GFileOutputStream * outstream ;
    gpointer currentfilename = g_malloc0(512);
    gpointer buffer = g_malloc0(512);

    if ( unzOpenCurrentFile(epub_document->epubDocument) != UNZ_OK )
    {
            return FALSE ;
    } 
        
    unzGetCurrentFileInfo64(epub_document->epubDocument,&info,currentfilename,512,NULL,0,NULL,0) ;
    directory = g_strrstr(currentfilename,"/") ;

    if ( directory != NULL )
        directory++; 

    gfilepath = g_string_new(epub_document->tmp_archive_dir) ;
    g_string_append_printf(gfilepath,"/%s",(gchar*)currentfilename);

    /*if we encounter a directory, make a directory inside our temporary folder.*/
    if (directory != NULL && *directory == '\0')
    {
        g_mkdir(gfilepath->str,0777);
    }
    else
    {
        outfile = g_file_new_for_path(gfilepath->str);
        outstream = g_file_create(outfile,G_FILE_CREATE_PRIVATE,NULL,error);
        while ( (writesize = unzReadCurrentFile(epub_document->epubDocument,buffer,512) ) != 0 )
        {
            if ( g_output_stream_write((GOutputStream*)outstream,buffer,writesize,NULL,error) == -1 )
            {
                return FALSE ;
            }
        }
        g_output_stream_close((GOutputStream*)outstream,NULL,error);
        g_object_unref(outfile) ;
        g_object_unref(outstream) ;
    }

    unzCloseCurrentFile (epub_document->epubDocument) ;
    g_string_free(gfilepath,TRUE);
    g_free(currentfilename);
    g_free(buffer);
    return TRUE;
}

static gboolean 
extract_epub_from_container (const gchar* uri, 
                             EpubDocument *epub_document,
                             GError ** error)
{
    GError* err = NULL ;
    GString * temporary_sub_directory ; 
    epub_document->archivename = g_filename_from_uri(uri,NULL,error);
    gchar* epubfilename ;
    if ( !epub_document->archivename )
    {
         if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not retrieve filename"));
        }
        return FALSE ;
    }

    epubfilename = g_strrstr(epub_document->archivename,"/");
    if ( *epubfilename == '/' )
    epubfilename++ ;

    temporary_sub_directory = g_string_new( epubfilename );
    g_string_append(temporary_sub_directory,"XXXXXX") ;

    epub_document->tmp_archive_dir = ev_mkdtemp(temporary_sub_directory->str,error) ;

    if (!epub_document->tmp_archive_dir) {
        return FALSE ;
    }

    g_string_free(temporary_sub_directory,TRUE);

    epub_document->epubDocument = unzOpen64(epub_document->archivename);

    if ( epub_document->epubDocument == NULL )
    {
        if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not open archive"));
        }
        return FALSE ;
    }
    if ( unzGoToFirstFile(epub_document->epubDocument) != UNZ_OK )
    {
        if (err) {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not extract archive"));
        }
        return FALSE ;
    }
    while ( TRUE )
    {
        if ( extract_one_file(epub_document,&err) == FALSE )
        {
            if (err) {
                g_propagate_error (error, err);
            } 
            else    {
                g_set_error_literal (error,
                             EV_DOCUMENT_ERROR,
                             EV_DOCUMENT_ERROR_INVALID,
                             _("could not extract archive"));
            }
			return FALSE;
        }   

        if ( unzGoToNextFile(epub_document->epubDocument) == UNZ_END_OF_LIST_OF_FILE )
            break ;
    }

    unzClose(epub_document->epubDocument);
    return TRUE ;
}


static gchar* 
get_uri_to_content(const gchar* uri,GError ** error,gchar* tmp_archive_dir)
{
    GError *   err = NULL ; 
    gchar*     containerpath = g_filename_from_uri(uri,NULL,&err);
    GString*   absolutepath ;
    gchar*     content_uri ;
    xmlNodePtr rootfileNode ;
    xmlChar*   relativepath;
    if ( !containerpath )
    {
        if (err) {
            g_propagate_error (error,err);
        } 
        else    {
            g_set_error_literal (error,
                                 EV_DOCUMENT_ERROR,
                                 EV_DOCUMENT_ERROR_INVALID,
                                 _("could not retrieve container file"));
        }
        return NULL ;
    }    

    if ( open_xml_document(containerpath) == FALSE )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("could not open container file"));
    
        return NULL ;
    }

    if ( set_xml_root_node((xmlChar*)"container") == FALSE)  {

        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("container file is corrupt"));    
        return NULL ;
    }

    if ( (rootfileNode = xml_get_pointer_to_node((xmlChar*)"rootfile",(xmlChar*)"media-type",(xmlChar*)"application/oebps-package+xml")) == NULL)
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file is invalid or corrput"));
        return NULL ;
    }
    
    relativepath = xml_get_data_from_node(rootfileNode,XML_ATTRIBUTE,(xmlChar*)"full-path") ;
   if ( relativepath == NULL )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file is corrupt,no container"));
        return NULL ;
    }
	absolutepath = g_string_new(tmp_archive_dir);
    g_string_append_printf(absolutepath,"/%s",relativepath);
    content_uri = g_filename_to_uri(absolutepath->str,NULL,&err);
    if ( !content_uri )  {
    if (err) {
            g_propagate_error (error,err);
        } 
        else    {
            g_set_error_literal (error,
                                 EV_DOCUMENT_ERROR,
                                 EV_DOCUMENT_ERROR_INVALID,
                                 _("could not retrieve container file"));
        }
        return NULL ;
    }
    g_string_free(absolutepath,TRUE);

    return content_uri ; 
}

static GList*
setup_document_content_list(const gchar* content_uri, GError** error,gchar *tmp_archive_dir)
{
    GList* newlist = NULL ;
    GError *   err = NULL ; 
    gint indexcounter= 1;
    xmlNodePtr manifest,spine,itemrefptr,itemptr ;
    gboolean errorflag = FALSE;

    gchar* relativepath ;
    GString* absolutepath = g_string_new(NULL);

    if ( open_xml_document(content_uri) == FALSE )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("could not parse content manifest"));
    
        return FALSE ;
    }
    if ( set_xml_root_node((xmlChar*)"package") == FALSE)  {

        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("content file is invalid"));    
        return FALSE ;
    }

    if ( ( spine = xml_get_pointer_to_node((xmlChar*)"spine",NULL,NULL) )== NULL )  
    {
         g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file has no spine"));    
        return FALSE ;
    }
    
    if ( ( manifest = xml_get_pointer_to_node((xmlChar*)"manifest",NULL,NULL) )== NULL )  
    {
         g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file has no manifest"));    
        return FALSE ;
    }

    xmlretval = NULL ;

    /*Get first instance of itemref from the spine*/
    xml_parse_children_of_node(spine,(xmlChar*)"itemref",NULL,NULL);
    
    if ( xmlretval != NULL )
        itemrefptr = xmlretval ;
    else
    {
        errorflag=TRUE;
    }
    /*Parse the spine for remaining itemrefs*/
    do
    {
        /*for the first time that we enter the loop, if errorflag is set we break*/
        if ( errorflag )
        {
            break;
        }
        if ( xmlStrcmp(itemrefptr->name,(xmlChar*)"itemref") == 0)
        {    
            contentListNode* newnode = g_malloc0(sizeof(newnode));    
            newnode->key = (gchar*)xml_get_data_from_node(itemrefptr,XML_ATTRIBUTE,(xmlChar*)"idref");
                   if ( newnode->key == NULL )
            {
                errorflag =TRUE;    
                break;
            }
            xmlretval=NULL ;
            xml_parse_children_of_node(manifest,(xmlChar*)"item",(xmlChar*)"id",(xmlChar*)newnode->key);
            
            if ( xmlretval != NULL )
            {
                itemptr = xmlretval ;
            }
            else
            {
                errorflag=TRUE;
                break;
            }
            relativepath = (gchar*)xml_get_data_from_node(itemptr,XML_ATTRIBUTE,(xmlChar*)"href");
            g_string_assign(absolutepath,tmp_archive_dir);
            g_string_append_printf(absolutepath,"/%s",relativepath);
            newnode->value = g_filename_to_uri(absolutepath->str,NULL,&err);
            if ( newnode->value == NULL )
            {
                errorflag =TRUE;    
                break;
            }

			newnode->index = indexcounter++ ;
            newlist = g_list_prepend(newlist,newnode);
        }
        itemrefptr = itemrefptr->next ;
    }
    while ( itemrefptr != NULL );

    if ( errorflag )
    {
        if ( err )
        {
            g_propagate_error(error,err);
        }
        else
        {            
            g_set_error_literal(error,
                                EV_DOCUMENT_ERROR,
                                EV_DOCUMENT_ERROR_INVALID,
                                _("Could not set up document tree for loading, some files missing"));
        }
        /*free any nodes that were set up and return empty*/
        g_string_free(absolutepath,TRUE);
        g_list_free_full(newlist,(GDestroyNotify)free_tree_nodes);
        return NULL ;
    }
	newlist = g_list_reverse(newlist);
    g_string_free(absolutepath,TRUE);
    return newlist ;

}

/* Callback function to free the contentlist.*/
static void
free_tree_nodes(gpointer data)
{
    contentListNode* dataptr = data ;
    g_free(dataptr->value);
    g_free(dataptr->key);
    g_free(dataptr);
}

static void
epub_document_init (EpubDocument *epub_document)
{
    epub_document->archivename = NULL ;
    epub_document->tmp_archive_dir = NULL ;
    epub_document->contentList = NULL ;
    epub_document->currentpageuri =  NULL;
}

static gboolean
epub_document_load (EvDocument* document,
                    const char* uri,
                    GError**    error)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	GError* err = NULL ;
	gchar* containeruri ;
	GString *containerpath ;
	gchar* contentOpfUri ;
	if ( check_mime_type (uri,&err) == FALSE )
	{
		/*Error would've been set by the function*/
		g_propagate_error(error,err);
		return FALSE;
	}

	extract_epub_from_container (uri,epub_document,&err);

	if ( err )
	{
		g_propagate_error( error,err );
		return FALSE;
	}
	/*FIXME : can this be different, ever?*/
	containerpath = g_string_new(epub_document->tmp_archive_dir);
	g_string_append_printf(containerpath,"/META-INF/container.xml");
	containeruri = g_filename_to_uri(containerpath->str,NULL,&err);

	if ( err )
	{
		g_propagate_error(error,err);
		return FALSE;
	}
	contentOpfUri = get_uri_to_content (containeruri,&err,epub_document->tmp_archive_dir);
	
	if ( contentOpfUri == NULL )
	{
		g_propagate_error(error,err);
		return FALSE;
	}

	xml_free_doc() ;
	
	epub_document->contentList = setup_document_content_list (contentOpfUri,&err,epub_document->tmp_archive_dir);

	if ( xmldocument != NULL )
		xml_free_doc ();
	
	if ( epub_document->contentList == NULL )
	{
		g_propagate_error(error,err);
		return FALSE;
	}

	return TRUE ;
}

static void
epub_document_finalize (GObject *object)
{
	EpubDocument *epub_document = EPUB_DOCUMENT (object);
	
	if (epub_document->epubDocument != NULL) {
		if (epub_remove_temporary_dir (epub_document->tmp_archive_dir) == -1)
			g_warning (_("There was an error deleting “%s”."),
				   epub_document->tmp_archive_dir);
		g_free (epub_document->tmp_archive_dir);
	}
	
	if ( epub_document->contentList ) {
               g_list_free_full(epub_document->contentList,(GDestroyNotify)free_tree_nodes);
	}

	g_free (epub_document->tmp_archive_dir);
	g_free (epub_document->currentpageuri);
	g_free (epub_document->archivename);

	G_OBJECT_CLASS (epub_document_parent_class)->finalize (object);
}

static EvDocumentInfo*
epub_document_get_info(EvDocument *document)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	GError *error = NULL ;
	gchar* infofile ;
	xmlNodePtr metanode ;
	GString* buffer ;
	gchar* archive_dir = epub_document->tmp_archive_dir;
	GString* containerpath = g_string_new(epub_document->tmp_archive_dir);
	g_string_append_printf(containerpath,"/META-INF/container.xml");
	gchar* containeruri = g_filename_to_uri(containerpath->str,NULL,&error);
	if ( error )
	{
		return NULL ;
	}
	gchar* uri = get_uri_to_content (containeruri,&error,archive_dir);
	if ( error )
	{
		return NULL ;
	}
	EvDocumentInfo* epubinfo = g_new0 (EvDocumentInfo, 1);
	
	if ( xmldocument != NULL )
		xml_free_doc();
	
	infofile = g_filename_from_uri(uri,NULL,&error);
	if ( error )
		return epubinfo;
	
	open_xml_document(infofile);

	set_xml_root_node((xmlChar*)"package");

	metanode = xml_get_pointer_to_node((xmlChar*)"title",NULL,NULL);
	if ( metanode == NULL )
	  epubinfo->title = NULL ;
	else
	  epubinfo->title = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);
	
	metanode = xml_get_pointer_to_node((xmlChar*)"creator",NULL,NULL);
	if ( metanode == NULL )
	  epubinfo->author = g_strdup("unknown");
	else
	  epubinfo->author = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	metanode = xml_get_pointer_to_node((xmlChar*)"subject",NULL,NULL);
	if ( metanode == NULL )
	   epubinfo->subject = g_strdup("unknown");
	else
	   epubinfo->subject = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	buffer = g_string_new((gchar*)xml_get_data_from_node (xmlroot,XML_ATTRIBUTE,(xmlChar*)"version"));
	g_string_prepend(buffer,"epub ");
	epubinfo->format = g_strdup(buffer->str);
	
	/*FIXME: Add more of these as you write the corresponding modules*/
	epubinfo->permissions = EV_DOCUMENT_PERMISSIONS_OK_TO_ADD_NOTES;

	epubinfo->layout = EV_DOCUMENT_LAYOUT_SINGLE_PAGE;

	metanode = xml_get_pointer_to_node((xmlChar*)"publisher",NULL,NULL);
	if ( metanode == NULL )
	   epubinfo->creator = g_strdup("unknown");
	else
	   epubinfo->creator = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	/*TODO : Add a function to get date*/
	g_free(uri);
	g_string_free(containerpath,TRUE);
	g_string_free(buffer,TRUE);
	return epubinfo ;
}

static EvPage*
epub_document_get_page(EvDocument *document,
                       gint index)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	EvPage* page = ev_page_new(index);
	contentListNode *listptr = g_list_nth_data (epub_document->contentList,index);
	page->backend_page = g_strdup(listptr->value);
	return page ;
}

static void
epub_document_class_init (EpubDocumentClass *klass)
{
	GObjectClass    *gobject_class = G_OBJECT_CLASS (klass);
	EvDocumentClass *ev_document_class = EV_DOCUMENT_CLASS (klass);

	gobject_class->finalize = epub_document_finalize;

	ev_document_class->load = epub_document_load;
	ev_document_class->save = epub_document_save;
	ev_document_class->get_n_pages = epub_document_get_n_pages;
/*	ev_document_class->wekit_render->render = epub_document_render;*/
	ev_document_class->get_info = epub_document_get_info; 
	ev_document_class->get_page = epub_document_get_page;
}
