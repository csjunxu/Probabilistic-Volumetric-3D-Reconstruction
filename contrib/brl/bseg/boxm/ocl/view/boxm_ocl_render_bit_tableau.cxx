#include "boxm_ocl_render_bit_tableau.h"
//:
// \file
#include <boxm/ocl/boxm_render_bit_scene_manager.h>
#include <boxm/ocl/boxm_ocl_utils.h>

#include <vpgl/vpgl_perspective_camera.h>
#include <vpgl/vpgl_calibration_matrix.h>
#include <vgui/internals/trackball.h>
#include <vgui/vgui_modifier.h>
#include <vcl_sstream.h>


//: Constructor
boxm_ocl_render_bit_tableau::boxm_ocl_render_bit_tableau()
{
  pbuffer_=0;
  ni_=640;
  nj_=480;
}

//: initialize tableau properties
bool boxm_ocl_render_bit_tableau::init(boxm_ocl_bit_scene * scene,
                                       unsigned ni, unsigned nj,
                                       vpgl_perspective_camera<double> * cam)
{
  //set image dimensions, camera and scene
  ni_=ni;
  nj_=nj;
  scene_=scene;
  cam_ = (*cam);
  default_cam_ = (*cam);
  do_init_ocl = true;
  return true;
}

bool boxm_ocl_render_bit_tableau::init_ocl()
{
  GLenum err = glewInit();
  if (GLEW_OK != err)
  {
    // Problem: glewInit failed, something is seriously wrong.
    vcl_cout<< "Error: "<<glewGetErrorString(err)<<vcl_endl;
  }

  vil_image_view<float> expected(ni_,nj_);

  //initialize the render manager
  boxm_render_bit_scene_manager* ray_mgr
      = boxm_render_bit_scene_manager::instance();
  cl_device_id device = ray_mgr->devices()[0];
  cl_platform_id platform_id[1];
  int status = clGetDeviceInfo(device,CL_DEVICE_PLATFORM,sizeof(platform_id),(void*) platform_id,NULL);
  if (!check_val(status, CL_SUCCESS, "boxm2_render Tableau::create_cl_gl_context CL_DEVICE_PLATFORM failed."))
    return 0;

  //create OpenCL context
  cl_context ComputeContext;
#ifdef WIN32
  cl_context_properties props[] =
  {
    CL_GL_CONTEXT_KHR, (cl_context_properties) wglGetCurrentContext(),
    CL_WGL_HDC_KHR, (cl_context_properties) wglGetCurrentDC(),
    CL_CONTEXT_PLATFORM, (cl_context_properties) platform_id[0],
    0
  };
  //create OpenCL context with display properties determined above
  ComputeContext = clCreateContext(props, 1, &device, NULL, NULL, &status);
#elif defined(__APPLE__) || defined(MACOSX)
  CGLContextObj kCGLContext = CGLGetCurrentContext();
  CGLShareGroupObj kCGLShareGroup = CGLGetShareGroup(kCGLContext);

  cl_context_properties props[] = {
    CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE, (cl_context_properties)kCGLShareGroup,
    CL_CONTEXT_PLATFORM, (cl_context_properties) platform_id[0],
    0
  };
  //create a CL context from a CGL share group - no GPU devices must be passed,
  //all CL compliant devices in the CGL share group will be used to create the context. more info in cl_gl_ext.h
  ComputeContext = clCreateContext(props, 0, 0, NULL, NULL, &status);
#else
  cl_context_properties props[] =
  {
      CL_GL_CONTEXT_KHR, (cl_context_properties)glXGetCurrentContext(),
      CL_GLX_DISPLAY_KHR, (cl_context_properties)glXGetCurrentDisplay(),
      CL_CONTEXT_PLATFORM, (cl_context_properties) platform_id[0],
      0
  };
  ComputeContext = clCreateContext(props, 1, &device, NULL, NULL, &status);
#endif

  if (status!=CL_SUCCESS) {
    vcl_cout<<"Error: Failed to create a compute CL/GL context!" << error_to_string(status) <<vcl_endl;
    return 0;
  }

#ifdef DEBUG
  // Check that CLGL devices are found
  unsigned int device_count;
  cl_device_id device_ids[16];

  vcl_size_t returned_size;
  err = clGetContextInfo(ComputeContext, CL_CONTEXT_DEVICES, sizeof(device_ids), device_ids, &returned_size);
  if (err)
  {
    vcl_cout << "Error: Failed to retrieve compute devices for context!\n";
    return 0;
  }

  device_count = returned_size / sizeof(cl_device_id);

  unsigned i = 0;
  bool device_found = 0;
  cl_device_type device_type;
  for (i = 0; i < device_count; i++)
  {
    clGetDeviceInfo(device_ids[i], CL_DEVICE_TYPE, sizeof(cl_device_type), &device_type, NULL);
    if (device_type == CL_DEVICE_TYPE_GPU)
    {
      device_found = true;
      break;
    }
  }

  if (!device_found)
  {
    vcl_cout<<"Error: Failed to locate compute device!\n";
    return 0;
  }
#endif

  //set the OpenCL context with display properties determined above
  ray_mgr->context_ = ComputeContext;

  //initialize ray trace using the input camera
  int bundle_dim = 8;
  ray_mgr->set_bundle_ni(bundle_dim);
  ray_mgr->set_bundle_nj(bundle_dim);
  ray_mgr->init_ray_trace(scene_, &cam_, expected, false);
  bool good=true;  good = good && ray_mgr->set_scene_data()
                               && ray_mgr->set_all_blocks()
                               && ray_mgr->set_scene_data_buffers()
                               && ray_mgr->set_tree_buffers();

  // run the raytracing
  good = good && ray_mgr->set_persp_camera(&cam_)
      && ray_mgr->set_persp_camera_buffers()
      && ray_mgr->set_input_image()
      && ray_mgr->set_input_image_buffers()
      && ray_mgr->set_image_dims_buffers();

  // delete old buffer
  if (pbuffer_) {
      clReleaseMemObject(ray_mgr->image_buf_);
      glDeleteBuffers(1, &pbuffer_);
  }

  //generate glBuffer, and bind to ray_mgr->image_gl_buf_
  glGenBuffers(1, &pbuffer_);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbuffer_);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, ray_mgr->wni() * ray_mgr->wnj() * sizeof(GLubyte) * 4, 0, GL_STREAM_DRAW);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  //create OpenCL buffer from GL PBO, and set kernel and arguments
  ray_mgr->image_gl_buf_ = clCreateFromGLBuffer(ray_mgr->context(),
                                                CL_MEM_WRITE_ONLY,
                                                pbuffer_,
                                                &status);
  ray_mgr->set_kernel();
  ray_mgr->set_args(0);
  ray_mgr->set_commandqueue();
  ray_mgr->set_workspace();

  return true;
}
//: Handles tableau events (drawing and keys)
bool boxm_ocl_render_bit_tableau::handle(vgui_event const &e)
{
  //draw handler - called on post_draw()
  if (e.type == vgui_DRAW)
  {
    if (do_init_ocl) {
      this->init_ocl();
      do_init_ocl = false;
    }

    //vcl_cout<<"Cam center: "<<cam_.get_camera_center()<<'\n'
    //        <<"stare point: "<<stare_point_<<vcl_endl;
    vcl_cout<<cam_<<vcl_endl;
    float gpu_time = this->render_frame();
    this->setup_gl_matrices();
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glRasterPos2i(0, 1);
    glPixelZoom(1,-1);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, pbuffer_);
    glDrawPixels(ni_, nj_, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
#if 0
    glPushMatrix();
    vnl_random rand;
    for (unsigned i=0;i<100000;i++)
    {
      glColor3f(rand.drand32(),rand.drand32(),rand.drand32());
      glBegin (GL_LINES);
      glVertex2f (rand.drand32(-1.0,1.0),rand.drand32(-1.0,1.0));
      glVertex2f (rand.drand32(-1.0,1.0), rand.drand32(-1.0,1.0));
      glEnd ();
    }
    glPopMatrix();
#endif // 0

    //calculate and write fps to status
    vcl_stringstream str;
    str<<"rendering at about "<< (1000.0f / gpu_time) <<" fps ";
    status_->write(str.str().c_str());

    return true;
  }

  if (boxm_vid_tableau::handle(e))
    return true;

  return false;
}

//: calls on ray manager to render frame into the pbuffer_
float boxm_ocl_render_bit_tableau::render_frame()
{
  boxm_render_bit_scene_manager* ray_mgr = boxm_render_bit_scene_manager::instance();
  cl_int status = clEnqueueAcquireGLObjects(ray_mgr->command_queue_, 1,
                                            &ray_mgr->image_gl_buf_ , 0, 0, 0);
  if (!check_val(status,CL_SUCCESS,"clEnqueueAcquireGLObjects failed. (gl_image)"+error_to_string(status)))
    return false;

  ray_mgr->set_persp_camera(&cam_);
  ray_mgr->write_persp_camera_buffers();
  ray_mgr->run();
  status = clEnqueueReleaseGLObjects(ray_mgr->command_queue_, 1, &ray_mgr->image_gl_buf_ , 0, 0, 0);
  clFinish( ray_mgr->command_queue_ );
  return ray_mgr->gpu_time();
}
