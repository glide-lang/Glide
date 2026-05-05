int main(int argc, char** argv) {
    __glide_args_init(argc, argv);
    __glide_sched_init();
    int __glide_rc = __glide_user_main(argc, argv);
    __glide_sched_shutdown();
    return __glide_rc;
}
