/**
 * @file jobs_execution.cpp
 * @brief Contains functions related to the execution of the jobs
 */

#include "jobs_execution.hpp"
#include "jobs.hpp"

#include <simgrid/plugins/energy.h>

#include <simgrid/msg.h>
#include <smpi/smpi.h>

XBT_LOG_NEW_DEFAULT_CATEGORY(jobs_execution, "jobs_execution"); //!< Logging

using namespace std;

int smpi_replay_process(int argc, char *argv[])
{
   /* for(int index = 0; index < argc; index++)
        XBT_DEBUG("smpi_replay_process, arg %d = '%s'", index, argv[index]);*/
    msg_sem_t sem = (msg_sem_t) MSG_process_get_data(MSG_process_self());
    if (sem != NULL)
    {
        MSG_sem_acquire(sem);
    }
    XBT_INFO("Launching smpi_replay_run");
    smpi_replay_run(&argc, &argv);
    XBT_INFO("smpi_replay_run finished");
    if (sem != NULL) {
        MSG_sem_release(sem);
    }
    return 0;
}

int execute_profile(BatsimContext *context,
                    const std::string & profile_name,
                    const SchedulingAllocation * allocation,
                    double * remaining_time)
{
    Workload * workload = context->workloads.at(allocation->job_id.workload_name);
    Job * job = workload->jobs->at(allocation->job_id.job_number);
    Profile * profile = workload->profiles->at(profile_name);
    int nb_res = job->required_nb_res;

    if (profile->type == ProfileType::MSG_PARALLEL_HOMOGENEOUS)
    {
        MsgParallelHomogeneousProfileData * data = (MsgParallelHomogeneousProfileData *)profile->data;

        double cpu = data->cpu;
        double com = data->com;

        // These amounts are deallocated by SG
        double * computation_amount = xbt_new(double, nb_res);
        double * communication_amount = NULL;
        if(com != 0.0)
            communication_amount = xbt_new(double, nb_res*nb_res);


        // Let us fill the local computation and communication matrices
        int k = 0;
        for (int y = 0; y < nb_res; ++y)
        {
            computation_amount[y] = cpu;
            if(communication_amount != NULL) {
                for (int x = 0; x < nb_res; ++x)
                {
                    if (x == y)
                        communication_amount[k++] = 0;
                    else
                        communication_amount[k++] = com;
                }
            }
        }

        string task_name = "phg " + to_string(job->number) + "'" + job->profile + "'";
        XBT_INFO("Creating task '%s'", task_name.c_str());

        msg_task_t ptask = MSG_parallel_task_create(task_name.c_str(),
                                                    nb_res, allocation->hosts.data(),
                                                    computation_amount,
                                                    communication_amount, NULL);

        double timeBeforeExecute = MSG_get_clock();
        XBT_INFO("Executing task '%s'", MSG_task_get_name(ptask));
        msg_error_t err = MSG_parallel_task_execute_with_timeout(ptask, *remaining_time);
        *remaining_time = *remaining_time - (MSG_get_clock() - timeBeforeExecute);

        int ret = 1;
        if (err == MSG_OK) {}
        else if (err == MSG_TIMEOUT)
            ret = 0;
        else
            xbt_die("A task execution had been stopped by an unhandled way (err = %d)", err);

        XBT_INFO("Task '%s' finished", MSG_task_get_name(ptask));
        MSG_task_destroy(ptask);
        return ret;
    }
    else if (profile->type == ProfileType::MSG_PARALLEL)
    {
        MsgParallelProfileData * data = (MsgParallelProfileData *)profile->data;

        // These amounts are deallocated by SG
        double * computation_amount = xbt_new(double, nb_res);
        double * communication_amount = xbt_new(double, nb_res*nb_res);

        // Let us retrieve the matrices from the profile
        memcpy(computation_amount, data->cpu, sizeof(double) * nb_res);
        memcpy(communication_amount, data->com, sizeof(double) * nb_res * nb_res);

        string task_name = "p " + to_string(job->number) + "'" + job->profile + "'";
        XBT_INFO("Creating task '%s'", task_name.c_str());

        msg_task_t ptask = MSG_parallel_task_create(task_name.c_str(),
                                                    nb_res, allocation->hosts.data(),
                                                    computation_amount,
                                                    communication_amount, NULL);

        double timeBeforeExecute = MSG_get_clock();
        XBT_INFO("Executing task '%s'", MSG_task_get_name(ptask));
        msg_error_t err = MSG_parallel_task_execute_with_timeout(ptask, *remaining_time);
        *remaining_time = *remaining_time - (MSG_get_clock() - timeBeforeExecute);

        int ret = 1;
        if (err == MSG_OK) {}
        else if (err == MSG_TIMEOUT)
            ret = 0;
        else
            xbt_die("A task execution had been stopped by an unhandled way (err = %d)", err);

        XBT_INFO("Task '%s' finished", MSG_task_get_name(ptask));
        MSG_task_destroy(ptask);
        return ret;
    }
    else if (profile->type == ProfileType::SEQUENCE)
    {
        xbt_die("Unhandled sequence profile type");
        SequenceProfileData * data = (SequenceProfileData *) profile->data;

        for (int i = 0; i < data->repeat; i++)
        {
            for (unsigned int j = 0; j < data->sequence.size(); j++)
            {
                if (execute_profile(context, data->sequence[j], allocation, remaining_time) == 0)
                    return 0;
            }
        }
        return 1;
    }
    else if (profile->type == ProfileType::DELAY)
    {
        DelayProfileData * data = (DelayProfileData *) profile->data;

        if (data->delay < *remaining_time)
        {
            XBT_INFO("Sleeping the whole task length");
            MSG_process_sleep(data->delay);
            XBT_INFO("Sleeping done");
            *remaining_time = *remaining_time - data->delay;
            return 1;
        }
        else
        {
            XBT_INFO("Sleeping until walltime");
            MSG_process_sleep(*remaining_time);
            XBT_INFO("Walltime reached");
            *remaining_time = 0;
            return 0;
        }
    }
    else if (profile->type == ProfileType::SMPI)
    {
        SmpiProfileData * data = (SmpiProfileData *) profile->data;
        msg_sem_t sem = MSG_sem_init(1);

        int nb_ranks = data->trace_filenames.size();

        // Let's use the default mapping is none is provided (round-robin on hosts, as we do not
        // know the number of cores on each host)
        if (job->smpi_ranks_to_hosts_mapping.empty())
        {
            job->smpi_ranks_to_hosts_mapping.resize(nb_ranks);
            int host_to_use = 0;
            for (int i = 0; i < nb_ranks; ++i)
            {
                job->smpi_ranks_to_hosts_mapping[i] = host_to_use;
                ++host_to_use %= job->required_nb_res; // ++ is done first
            }
        }

        xbt_assert(nb_ranks == (int) job->smpi_ranks_to_hosts_mapping.size(),
                   "Invalid job %s: SMPI ranks_to_host mapping has an invalid size, as it should "
                   "use %d MPI ranks but the ranking states that there are %d ranks.",
                   job->id, nb_ranks, (int) job->smpi_ranks_to_hosts_mapping.size());

        for (int i = 0; i < nb_ranks; ++i)
        {
            char *str_instance_id = NULL;
            int ret = asprintf(&str_instance_id, "%s!%d", job->workload->name.c_str(), job->number);
            xbt_assert(ret != -1, "asprintf failed (not enough memory?)");

            char *str_rank_id  = NULL;
            ret = asprintf(&str_rank_id, "%d", i);
            xbt_assert(ret != -1, "asprintf failed (not enough memory?)");

            char *str_pname = NULL;
            ret = asprintf(&str_pname, "%d_%d", job->number, i);
            xbt_assert(ret != -1, "asprintf failed (not enough memory?)");

            char **argv = xbt_new(char*, 5);
            argv[0] = xbt_strdup("1"); // Fonction_replay_label (can be ignored, for log only),
            argv[1] = str_instance_id; // Instance Id (application) job_id is used
            argv[2] = str_rank_id;     // Rank Id
            argv[3] = xbt_strdup((char*) data->trace_filenames[i].c_str());
            argv[4] = xbt_strdup("0"); //

            msg_host_t host_to_use = allocation->hosts[job->smpi_ranks_to_hosts_mapping[i]];
            msg_sem_t sem_to_use = NULL;

            XBT_INFO("Hello!");

            if (i == 0)
                sem_to_use = sem;

            MSG_process_create_with_arguments(str_pname, smpi_replay_process, sem_to_use,
                                              host_to_use, 5, argv);

            // todo: avoid memory leaks
            free(str_pname);

        }
        MSG_sem_acquire(sem);
        free(sem);
        return 1;
    }

    if (profile->type == ProfileType::MSG_PARALLEL_HOMOGENEOUS_PFS0)
    {
        MsgParallelHomogeneousPFS0ProfileData * data = (MsgParallelHomogeneousPFS0ProfileData *)profile->data;

        double cpu = 0.0;
        double size = data->size;
        std::vector<msg_host_t> hosts_pfs0(allocation->hosts);

        // Add the pfs_machine
        hosts_pfs0.push_back((context->machines.pfs_machine())->host);

        nb_res = nb_res + 1;

        // These amounts are deallocated by SG
        double * computation_amount = xbt_new(double, nb_res);
        double * communication_amount = NULL;
        if(size != 0.0)
            communication_amount = xbt_new(double, nb_res*nb_res);


        // Let us fill the local computation and communication matrices
        int k = 0;
        for (int y = 0; y < nb_res; ++y)
        {
            computation_amount[y] = cpu;
            if(communication_amount != NULL) {
                for (int x = 0; x < nb_res; ++x)
                {
                    if ((x != nb_res-1) || (x == y))
                        communication_amount[k++] = 0;
                    else
                        communication_amount[k++] = size;
                }
            }
        }

        string task_name = "phg " + to_string(job->number) + "'" + job->profile + "'";
        XBT_INFO("Creating task '%s'", task_name.c_str());

        msg_task_t ptask = MSG_parallel_task_create(task_name.c_str(),
                                         nb_res, allocation->hosts.data(),
                                         computation_amount,
                                         communication_amount, NULL);

        double timeBeforeExecute = MSG_get_clock();
        XBT_INFO("Executing task '%s'", MSG_task_get_name(ptask));
        msg_error_t err = MSG_parallel_task_execute_with_timeout(ptask, *remaining_time);
        *remaining_time = *remaining_time - (MSG_get_clock() - timeBeforeExecute);

        int ret = 1;
        if (err == MSG_OK) {}
        else if (err == MSG_TIMEOUT)
            ret = 0;
        else
            xbt_die("A task execution had been stopped by an unhandled way (err = %d)", err);

        XBT_INFO("Task '%s' finished", MSG_task_get_name(ptask));
        MSG_task_destroy(ptask);
        return ret;
    }
    else
        xbt_die("Cannot execute job %s: the profile type '%s' is unknown",
                job->id.c_str(), job->profile.c_str());

    return 0;
}

int lite_execute_job_process(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    // Retrieving input parameters
    ExecuteJobProcessArguments * args = (ExecuteJobProcessArguments *) MSG_process_get_data(MSG_process_self());

    Job * job = args->context->workloads.job_at(args->allocation->job_id);
    job->starting_time = MSG_get_clock();
    job->allocation = args->allocation->machine_ids;
    double remaining_time = (double) job->walltime;

    // Job computation
    args->context->machines.update_machines_on_job_run(job, args->allocation->machine_ids, args->context);
    if (execute_profile(args->context, job->profile, args->allocation, &remaining_time) == 1)
    {
        XBT_INFO("Job %s finished in time", job->id.c_str());
        job->state = JobState::JOB_STATE_COMPLETED_SUCCESSFULLY;
    }
    else
    {
        XBT_INFO("Job %s had been killed (walltime %g reached)",
                 job->id.c_str(), (double)job->walltime);
        job->state = JobState::JOB_STATE_COMPLETED_KILLED;
        if (args->context->trace_schedule)
            args->context->paje_tracer.add_job_kill(job, args->allocation->machine_ids, MSG_get_clock(), true);
    }

    args->context->machines.update_machines_on_job_end(job, args->allocation->machine_ids, args->context);
    job->runtime = MSG_get_clock() - job->starting_time;

    delete args->allocation;
    delete args;

    return 0;
}

int execute_job_process(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    // Retrieving input parameters
    ExecuteJobProcessArguments * args = (ExecuteJobProcessArguments *) MSG_process_get_data(MSG_process_self());

    Workload * workload = args->context->workloads.at(args->allocation->job_id.workload_name);
    Job * job = workload->jobs->at(args->allocation->job_id.job_number);
    job->starting_time = MSG_get_clock();
    job->allocation = args->allocation->machine_ids;
    double remaining_time = (double)job->walltime;

    // If energy is enabled, let us compute the energy used by the machines before running the job
    if (args->context->energy_used)
    {
        job->consumed_energy = 0;

        for (auto it = job->allocation.elements_begin(); it != job->allocation.elements_end(); ++it)
        {
            int machine_id = *it;
            Machine * machine = args->context->machines[machine_id];
            job->consumed_energy += sg_host_get_consumed_energy(machine->host);
        }

        // Let's trace the consumed energy
        args->context->energy_tracer.add_job_start(MSG_get_clock(), job->number);
    }

    // Job computation
    args->context->machines.update_machines_on_job_run(job,
                                                       args->allocation->machine_ids,
                                                       args->context);
    if (execute_profile(args->context, job->profile, args->allocation, &remaining_time) == 1)
    {
        XBT_INFO("Job %s finished in time", job->id.c_str());
        job->state = JobState::JOB_STATE_COMPLETED_SUCCESSFULLY;
    }
    else
    {
        XBT_INFO("Job %s had been killed (walltime %g reached)",
                 job->id.c_str(), (double) job->walltime);
        job->state = JobState::JOB_STATE_COMPLETED_KILLED;
        if (args->context->trace_schedule)
            args->context->paje_tracer.add_job_kill(job,
                                                    args->allocation->machine_ids,
                                                    MSG_get_clock(),
                                                    true);
    }

    args->context->machines.update_machines_on_job_end(job,
                                                       args->allocation->machine_ids,
                                                       args->context);
    job->runtime = (Rational)MSG_get_clock() - job->starting_time;
    // dirty hack, should be removed once SMPI works correctly
    if (workload->profiles->at(job->profile)->type == ProfileType::SMPI)
        job->runtime = max(job->runtime, Rational(1e-5));

    xbt_assert(job->runtime > 0, "The execution of job '%s' resulted in a null execution time. "
               "This might be caused by MSG_clock() precision but also by a bad job profile.",
               job->id.c_str());

    // If energy is enabled, let us compute the energy used by the machines after running the job
    if (args->context->energy_used)
    {
        long double consumed_energy_before = job->consumed_energy;
        job->consumed_energy = 0;


        for (auto it = job->allocation.elements_begin(); it != job->allocation.elements_end(); ++it)
        {
            int machine_id = *it;
            Machine * machine = args->context->machines[machine_id];
            job->consumed_energy += sg_host_get_consumed_energy(machine->host);
        }

        // The consumed energy is the difference (consumed_energy_after_job - consumed_energy_before_job)
        job->consumed_energy = job->consumed_energy - consumed_energy_before;

        // Let's trace the consumed energy
        args->context->energy_tracer.add_job_end(MSG_get_clock(), job->number);
    }

    // Let us tell the server that the job completed
    JobCompletedMessage * message = new JobCompletedMessage;
    message->job_id = args->allocation->job_id;

    send_message("server", IPMessageType::JOB_COMPLETED, (void*)message);
    delete args->allocation;
    delete args;

    return 0;
}

int waiter_process(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    WaiterProcessArguments * args = (WaiterProcessArguments *) MSG_process_get_data(MSG_process_self());

    double curr_time = MSG_get_clock();

    if (curr_time < args->target_time)
    {
        double time_to_wait = args->target_time - curr_time;
        // Sometimes time_to_wait is so small that it does not affect MSG_process_sleep. The value of 1e-5 have been found on trial-error.
        if(time_to_wait < 1e-5)
            time_to_wait = 1e-5;
        XBT_INFO("Sleeping %g seconds to reach time %g", time_to_wait, args->target_time);
        MSG_process_sleep(time_to_wait);
        XBT_INFO("Sleeping done");
    }
    else
        XBT_INFO("Time %g is already reached, skipping sleep", args->target_time);

    send_message("server", IPMessageType::WAITING_DONE);
    delete args;

    return 0;
}
