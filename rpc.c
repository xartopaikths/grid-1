void handle_rpc(int connection) {
  int rpc;
  char host[INET_ADDRSTRLEN];
  get_ip(connection, host);
  safe_recv(connection, &rpc, sizeof(rpc));
  printf(BAR);
  printf("RPC %s from %s\n", which_rpc(rpc), host);
  printf(BAR);
  switch(rpc) {
  case SEND_SERVERS:
    rpc_send_servers(connection);
    break;
  case SERVE_JOB:
    rpc_serve_job(connection);
    break;
  case JOB_COMPLETE:
    rpc_inform_of_completion(connection);
    break;
  case RECEIVE_UPDATE:
    rpc_receive_update(connection);
    break;
  case RECEIVE_JOB_COPY:
    rpc_receive_job_copy(connection);
    break;
  case INFORM_OF_FAILURE:
    rpc_inform_of_failure(connection);
    break;
  case REQUEST_ADD_LOCK:
    rpc_request_add_lock(connection);
    break;
  case ADD_JOB:
    rpc_add_job(connection);
    break;
  case ANNOUNCE:
    rpc_receive_announce(connection);
    break;
  case UNLOCK:
    rpc_unlock(connection);
    break;
  case TRANSFER_JOB:
    rpc_transfer_job(connection);
    break;
  default:
    break;
  }
  close(connection);
  print_server_list();
}

char *which_rpc(int rpc) {
  switch(rpc) {
  case SEND_SERVERS:
    return SEND_SERVERS_S;
    break;
  case SERVE_JOB:
    return SERVE_JOB_S;
    break;
  case JOB_COMPLETE:
    return JOB_COMPLETE_S;
    break;
  case RECEIVE_UPDATE:
    return RECEIVE_UPDATE_S;
    break;
  case RECEIVE_JOB_COPY:
    return RECEIVE_JOB_COPY_S;
    break;
  case INFORM_OF_FAILURE:
    return INFORM_OF_FAILURE_S;
    break;
  case REQUEST_ADD_LOCK:
    return REQUEST_ADD_LOCK_S;
  case ADD_JOB:
    return ADD_JOB_S;
    break;
  case ANNOUNCE:
    return ANNOUNCE_S;
    break;
  case UNLOCK:
    return UNLOCK_S;
    break;
  case TRANSFER_JOB:
    return TRANSFER_JOB_S;
    break;
  default:
    break;
  }
}

void rpc_transfer_job(int connection) {
  int err;
  job *incoming;
  incoming = malloc(sizeof(job));
  err = safe_recv(connection, incoming, sizeof(job));
  if(err < 0) {
    problem("Job transfer failed.\n");
  } else {
    add_to_queue(incoming, activeQueue); //will need to change later for dependencies
  }
}

void rpc_receive_announce(int connection) {
  int status;
  host_port *incoming;
  incoming = malloc(sizeof(host_port));
  status = safe_recv(connection, incoming, sizeof(host_port));
  add_host_to_list_by_location(incoming, server_list);
}

void add_host_to_list_by_location(host_port *host, host_list *list) {
  host_list_node *runner = list->head;
  while(runner->next->host->location < host->location) {
    runner = runner->next;
  }
  add_to_host_list(host, runner);
}

void rpc_request_add_lock(int connection) {
  int result = FAILURE;
  pthread_mutex_lock(&d_add_mutex);
  if(!d_add_lock) {
    d_add_lock = LOCKED;
    get_ip(connection, who);
    result = OKAY;
  }
  pthread_mutex_unlock(&d_add_mutex);
  safe_send(connection, &result, sizeof(int));
}

void rpc_unlock(int connection) {
  int result = FAILURE;
  char ip[INET_ADDRSTRLEN];
  get_ip(connection, ip);
  pthread_mutex_lock(&d_add_mutex);
  if(!strcmp(who, ip)) {
    d_add_lock = UNLOCKED;
    result = OKAY;
  }
  pthread_mutex_unlock(&d_add_mutex);
  safe_send(connection, &result, sizeof(int));
}

void rpc_send_servers(int connection) {
  send_host_list(connection, server_list);
}


void rpc_receive_update(int connection){
  host_list *old = server_list;
  int err;
  err = receive_host_list(connection, &server_list);
  if(err < 0) { 
    problem("Receive Update failed\n");
    return; //Fix?
  }
  err = OKAY;
  safe_send(connection, &err, sizeof(int));
}

void rpc_add_job(int connection) {
  int num_files, i, err;
  data_size *files;
  job *new;
  new = malloc(sizeof(job));
  err = safe_recv(connection, new, sizeof(job));
  if(err < 0) problem("Recv job failed, %d\n",err);
  
  safe_recv(connection, &num_files, sizeof(int));
  files = malloc(sizeof(data_size)*num_files);
  for(i = 0; i < num_files; i++) {
    receive_file(connection, &files[i]);
  }
  i = get_job_id(new);
  write_files(new, num_files, files);
  send_meta_data(new);
  safe_send(connection, &i, sizeof(int));
  for(i = 0; i < num_files; i++) {
    free(files[i].data);
  }
  free(files);
}

int send_meta_data(job *ajob) {
  host_list_node *dest = determine_ownership(ajob);
  if(dest->host == my_hostport) {
    
  } else {
    transfer_job(dest->host, ajob);
  }
}

host_list_node *determine_ownership(job *ajob) {
  char buffer[BUFFER_SIZE];
  int _hash;
  host_list_node *runner, *prev;
  sprintf(buffer, "%d", ajob->id);
  _hash = hash(buffer);
  runner = server_list->head;
  while(runner->host->location <= _hash) {
    prev = runner;
    runner = runner->next;
  }
  return prev;
}

int write_files(job *ajob, int num_files, data_size *files) {
  char buffer[BUFFER_SIZE], back[BUFFER_SIZE];
  FILE *temp;
  int i;
  sprintf(buffer,"./jobs/%d/", ajob->id); 
  if(mkdir(buffer, S_IRWXU)) {
    if(errno == EEXIST) {
      problem("directory already exists...\n");
    } else {
      problem("mkdir failed with %d\n", errno);
    }
  }
  for(i = 0; i < num_files; i++) {
    printf("%s\n", files[i].name); 
    sprintf(buffer,"jobs/%d/%s", ajob->id, files[i].name);
    temp = NULL;
    temp = fopen(buffer, "w");
    if(temp) {
      fwrite(files[i].data, files[i].size, 1, temp);
    } else {
      problem("failed to open file %s, errno: %d\n", files[i].name, errno);
    }
    fclose(temp);
  }
}

int get_job_id(job *ajob) {
  pthread_mutex_lock(&count_mutex);
  ajob->id = my_hostport->location + counter;
  counter++;
  pthread_mutex_unlock(&count_mutex);
  return ajob->id;
}

int receive_file(int connection, data_size *file) {
  int err;
  err = safe_recv(connection, &(file->size), sizeof(size_t));
  if(err < 0) problem("Recv failed size\n");
  file->data = malloc(file->size);
  err = safe_recv(connection, file->data, file->size);
  if(err < 0) problem("Recv failed data\n");
  err = safe_recv(connection, file->name, MAX_ARGUMENT_LEN*sizeof(char));
  if(err < 0) problem("Recv failed name\n");
}

void rpc_serve_job(int connection) {
  int msg = FAILURE;
  job *ajob = get_local_job();
  if(ajob) {
    msg = OKAY;
    do_rpc(&msg); //not doing an rpc just shorthand for sending an int
    send_job(ajob, connection);
  }
  do_rpc(&msg);
}

void send_job(job *job_to_send,int connection) {
  int err = OKAY;
  err = safe_send(connection,&job_to_send,sizeof(job));
  
}

void rpc_receive_job_copy(int connection){
}

void rpc_request_job(int connection) { 
  
}

void rpc_inform_of_completion(int connection) {
  int err = OKAY;
  int jobid;
  err = safe_recv(connection,&jobid,sizeof(int));  
  if (err < OKAY) return;

  update_q_job_complete(jobid,backupQueue);
}


void rpc_inform_of_failure(int connection) {
  int err = OKAY;
  host_port *received_hp, *failed_host;
  received_hp = malloc(sizeof(host_port));
  err = safe_recv(connection,received_hp,sizeof(host_port));  
  if (err < OKAY){
    problem("did not receive failed host");
    return;
  }

  failed_host = find_host_in_list(received_hp->ip,server_list);
  local_handle_failure(failed_host);
}

// If a job is complete then we need to update the queue to make jobs that depended on that job available.

void update_q_job_complete (int jobid, queue *Q) {
   job_list_node *current;
   current = Q->head;
   while(current != NULL) {
       if (contains(current->entry, jobid)) {
	 remove_dependency(current->entry, jobid);
         //check_avail(current->entry);
       }
       current = current->next;
   } 
}

int contains(job *current, int jobid) {
  int i = 0;
  while (i < current->argc) {
    if (jobid == current->dependent_on[i]) return 1;
    i++;
  }
  return 0;
}


void remove_dependency(job *current, int jobid) {
  int i = 0;
  while (i < current->argc) {
    if (jobid == current->dependent_on[i]) {
      current->dependent_on[i] = 0;
      break;
    }
    i++;
  }
}

void add_to_queue(job *addJob, queue *Q) {
  job_list_node *n; 
  n = (job_list_node *)malloc(sizeof(job_list_node));
  n->entry = addJob;
  n->next = Q->head;
  Q->head = n;
}
