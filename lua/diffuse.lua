-- Load a static scene
nwm_init()
window = nwm_create_window(800, 600)

camera = ngl_new_camera(-20, 18, 50)
model = ngl_load_obj("../obj/c004.obj")
shader = ngl_load_shader("../shader/diffuse.vert", "../shader/basic.frag")

while not nwm_window_should_close(window) do
    ngl_clear(0.2, 0.2, 0.2, 1)
    ngl_draw_model(camera, model, shader)

    nwm_poll_events()
    nwm_swap_buffers(window)
end

nwm_destroy_window(window)
nwm_terminate()