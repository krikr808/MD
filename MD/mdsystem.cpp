////////////////////////////////////////////////////////////////
// INCLUDE FILES
////////////////////////////////////////////////////////////////

// Standard includes
#include <stdexcept>
using std::runtime_error;
#include <cstdlib>
#include <iostream>
#include <iomanip>
using std::endl;
#include <fstream>
using std::ofstream;

// Own includes
#include "mdsystem.h"

////////////////////////////////////////////////////////////////
// CONSTRUCTOR
////////////////////////////////////////////////////////////////

mdsystem::mdsystem()
{
    operating = false;
    start_operation();
    abort_activities_requested = false;
    system_initialized = false;
    finish_operation();
}

////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS
////////////////////////////////////////////////////////////////

void mdsystem::set_event_callback(callback<void (*)(void*)> event_callback_in)
{
    start_operation();
    event_callback = event_callback_in;
    finish_operation();
}

void mdsystem::set_output_callback(callback<void (*)(void*, string)> output_callback_in)
{
    start_operation();
    output_callback = output_callback_in;
    finish_operation();
}

void mdsystem::init(uint num_particles_in, ftype sigma_in, ftype epsilon_in, ftype inner_cutoff_in, ftype outer_cutoff_in, ftype particle_mass_in, ftype dt_in, uint ensemble_size_in, uint sample_period_in, ftype temperature_in, uint num_timesteps_in, ftype lattice_constant_in, uint lattice_type_in, ftype desired_temp_in, ftype thermostat_time_in, ftype dEp_tolerance_in, ftype default_impulse_response_decay_time_in, uint default_num_times_filtering_in, bool slope_compensate_by_default_in, bool thermostat_on_in, bool diff_c_on_in, bool Cv_on_in, bool pressure_on_in, bool msd_on_in, bool Ep_on_in, bool Ek_on_in)
{
    // The system is *always* operating when running non-const functions
    start_operation();

#if THERMOSTAT == LASSES_THERMOSTAT
    thermostat_value = 0;
#endif
    /*
     * Copy in parameters to member variables
     */
    // Conversion units
    particle_mass_in_kg = particle_mass_in;
    sigma_in_m          = sigma_in;
    epsilon_in_j        = epsilon_in;

    // Copy rest of the parameters
    // Lengths
    lattice_constant = lattice_constant_in;
    outer_cutoff     = outer_cutoff_in;
    inner_cutoff     = inner_cutoff_in;
    // Temperatures
    init_temp        = temperature_in;
    desired_temp     = desired_temp_in;
    // Times
    dt               = dt_in;           // Delta time, the time step to be taken when solving the diff.eq.
    thermostat_time  = thermostat_time_in;
    default_impulse_response_decay_time = default_impulse_response_decay_time_in;
    // Unitless
    sampling_period  = sample_period_in;
#if FILTER == KRISTOFERS_FILTER
    default_num_times_filtering = default_num_times_filtering_in;
    slope_compensate_by_default = slope_compensate_by_default_in;
#elif FILTER == EMILS_FILTER
    ensemble_size    = ensemble_size_in;
#endif
    lattice_type     = lattice_type_in; // One of the supported lattice types listed in enum_lattice_types
    dEp_tolerance    = dEp_tolerance_in;
    diff_c_on        = diff_c_on_in;
    Cv_on            = Cv_on_in;
    pressure_on      = pressure_on_in;
    msd_on           = msd_on_in;
    Ep_on            = Ep_on_in;
    Ek_on            = Ek_on_in;

    //TODO: Make sure all non-unitless parameters are converted to reduced units
    // Convert in parameters to reduced units before using them
    /*
     * Reduced units
     *
     * Length unit: sigma
     * Energy unit: epsilon
     * Mass unit: particle mass
     * Temperature unit: epsilon/KB
     *
     * Time unit: sigma * (particle mass / epsilon)^.5
     */
    // Masses /= particle_mass_in_kg;
    // Lengths /= sigma_in_m;
    lattice_constant /= sigma_in_m;
    inner_cutoff     /= sigma_in_m;
    outer_cutoff     /= sigma_in_m;
    // Energies /= epsilon_in_j;
    // Temperatures *= P_KB / epsilon_in_j;
    init_temp        *= P_SI_KB / epsilon_in_j;
    desired_temp     *= P_SI_KB / epsilon_in_j;
    // Times /= sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j);
    dt               /= sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j);
    thermostat_time  /= sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j);
    default_impulse_response_decay_time /= sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j);
    // Pressures *= sigma_in_m * sigma_in_m * sigma_in_m / epsilon_in_j;

    sqr_outer_cutoff = outer_cutoff*outer_cutoff; // Parameter for the Verlet list
    sqr_inner_cutoff = inner_cutoff*inner_cutoff; // Parameter for the Verlet list

    // Prevent unstabilities because of too small thermostat_time
    if (thermostat_time < sampling_period * dt) {
        thermostat_time = sampling_period * dt;
    }

    // Initializations miscellaneous variables
    loop_num = 0;
#if FILTER == KRISTOFERS_FILTER
    num_time_steps = ((num_timesteps_in - 1) / sampling_period + 1) * sampling_period; // Make the smallest multiple of sample_period that has at least the specified size
    num_sampling_points = num_time_steps/sampling_period + 1;
    output << "num_time_steps: " << num_time_steps << endl;
    output << "num_sampling_points: " << num_sampling_points << endl;
#elif FILTER == EMILS_FILTER
    num_sampling_points = (num_timesteps_in - 1) / sampling_period + 2;
    uint num_ensambles = (num_sampling_points - 1) / ensemble_size + 1;
    num_sampling_points = num_ensambles * ensemble_size;
    num_time_steps = (num_sampling_points - 1)*sampling_period;
    output << "num_time_steps: " << num_time_steps << endl;
    output << "num_sampling_points: " << num_sampling_points << endl;
    output << "num_ensambles: " << num_ensambles << endl;
#endif

    insttemp             .resize(num_sampling_points);
    instEk               .resize(num_sampling_points);
    instEp               .resize(num_sampling_points);
    instEc               .resize(num_sampling_points);
    thermostat_values    .resize(num_sampling_points);
    msd                  .resize(num_sampling_points);
    diffusion_coefficient.resize(num_sampling_points);
    distance_force_sum   .resize(num_sampling_points);

    if (lattice_type == LT_FCC) {
        box_size_in_lattice_constants = int(pow(ftype(num_particles_in / 4.0 ), ftype( 1.0 / 3.0 )));
        num_particles = 4*box_size_in_lattice_constants*box_size_in_lattice_constants*box_size_in_lattice_constants;   // Calculate the new number of atoms; all can't fit in the box since n is an integer
    }
    else {
        output << "Lattice type unknown" << endl;
        goto operation_finished;
    }
    output << "num_particles: " << num_particles << endl;

    // Box
    box_size = lattice_constant*box_size_in_lattice_constants;
    pos_half_box_size = 0.5f * box_size;
    neg_half_box_size = -pos_half_box_size;

    // Thermostat
    thermostat_on = thermostat_on_in;
    equilibrium_reached = false;

    // Call other initialization functions
    init_particles();
    create_verlet_list();
    calculate_potential_energy_cutoff();

    // Flag the system as initialized
    system_initialized = true;

operation_finished:
    // Finish the operation
    finish_operation();
}

void mdsystem::run_simulation()
{
    // The system is *always* operating when running non-const functions
    start_operation();
    /*
     * All variables define in this function has to defined here since we use
     * goto's.
     */
    // Open the output files. They work like cin
    ofstream out_filter_test_data1;
    ofstream out_filter_test_data2;
    ofstream out_filter_test_data3;
    ofstream out_etot_data    ;
    ofstream out_ep_data      ;
    ofstream out_ek_data      ;
    ofstream out_cv_data      ;
    ofstream out_temp_data    ;
    ofstream out_therm_data   ;
    ofstream out_msd_data     ;
    ofstream out_diff_c_data  ;
    ofstream out_cohe_data    ;
    ofstream out_pressure_data;
    // For calculating the average specific heat
    ftype Cv_sum;
    uint  Cv_num;
    // For shifting the potential energy
    ftype Ep_shift;

    // Start simulating
    enter_loop_number(0);
    calculate_forces();
    measure_unfiltered_properties();
    while (loop_num < num_time_steps) {
        // Check if the simulation has been requested to abort
        if (abort_activities_requested) {
            goto operation_finished;
        }

        if (!sampling_in_this_loop) {
            calculate_forces();
        }

        // Evolve the system in time
        leapfrog(); // This function includes the force calculation

        if (sampling_in_this_loop) {
            measure_unfiltered_properties();
        }

        // Process events
        print_output_and_process_events();
    }

    // Now the filtered properties can be calculated
    calculate_filtered_properties();
    output << "*******************" << endl;
    output << "Simulation completed." << endl;

    /*
     * TODO: The following code should be moved into another public function.
     * This function should *just* run the simulation since that is what it
     * says it does.
     */

    /*
    // Lengths * sigma_in_m;
    // Temperatures * epsilon_in_j/P_KB;
    // Times * sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j);
    // Pressures * epsilon_in_j / (sigma_in_m * sigma_in_m * sigma_in_m);
    */
    Ep_shift = -instEp[0];
    output << "Opening output files..." << endl;
    if (!(open_ofstream_file(out_filter_test_data1, "FilterTest1.dat") &&
          open_ofstream_file(out_filter_test_data2, "FilterTest2.dat") &&
          open_ofstream_file(out_filter_test_data3, "FilterTest3.dat") &&
          open_ofstream_file(out_etot_data    , "TotalEnergy.dat") &&
          open_ofstream_file(out_ep_data      , "Potential.dat"  ) &&
          open_ofstream_file(out_ek_data      , "Kinetic.dat"    ) &&
          open_ofstream_file(out_cv_data      , "Cv.dat"         ) &&
          open_ofstream_file(out_temp_data    , "Temperature.dat") &&
          open_ofstream_file(out_therm_data   , "Thermostat.dat" ) &&
          open_ofstream_file(out_msd_data     , "MSD.dat"        ) &&
          open_ofstream_file(out_diff_c_data  , "diff_coeff.dat" ) &&
          open_ofstream_file(out_pressure_data,"Pressure.dat"    ) &&
          open_ofstream_file(out_cohe_data    , "cohesive.dat"   )
          )) {
        cerr << "Error: Output files could not be opened" << endl;
    }
    else {
        output << "Writing to output files..." << endl;
        print_output_and_process_events();

        /////////Start writing files////////////////////////////////////////////////////////

        vector<ftype> dirac_impulse1(num_sampling_points);
        vector<ftype> dirac_impulse2(num_sampling_points);
        vector<ftype> line(num_sampling_points);

        dirac_impulse1[int(default_impulse_response_decay_time/dt/2)] = 1;
        dirac_impulse2[num_sampling_points - 1 - int(default_impulse_response_decay_time/dt/4)] = 1;
        for (int i = 0; i < int(num_sampling_points); i++) line[i] = i - int(num_sampling_points)/3;
        vector<ftype> filtered_dirac_impulse1;
        vector<ftype> filtered_dirac_impulse2;
        vector<ftype> filtered_line;
        filter(dirac_impulse1, filtered_dirac_impulse1, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
        filter(dirac_impulse2, filtered_dirac_impulse2, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
        filter(line          , filtered_line          , default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);

        //Tests
        for (uint i = 0; i < filtered_dirac_impulse1.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_filter_test_data1  << setprecision(9) << filtered_dirac_impulse1[i] << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < filtered_dirac_impulse2.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_filter_test_data2  << setprecision(9) << filtered_dirac_impulse2[i] << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < filtered_line.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_filter_test_data3  << setprecision(9) << filtered_line[i] << endl;
            // Process events
            process_events();
        }
        // Lengths * sigma_in_m/P_ANGSTROM [Angstrom]
        // Energies * epsilon_in_j/P_EV [eV]
        for (uint i = 0; i < Ek.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_etot_data  << setprecision(9) << (Ek[i] + (Ep[i] + Ep_shift))*epsilon_in_j/P_SI_EV << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < Ek.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_ek_data    << setprecision(9) << Ek[i]*epsilon_in_j/P_SI_EV << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < Ep.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_ep_data    << setprecision(9) << (Ep[i] + Ep_shift)*epsilon_in_j/P_SI_EV << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < cohesive_energy.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_cohe_data  << setprecision(9) << cohesive_energy[i]*epsilon_in_j/P_SI_EV << endl;
            // Process events
            process_events();
        }
        // Masses * particle_mass_in_kg [kg]
        // Times * sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j) [s]
        // Temperatures * epsilon_in_j/P_KB [K]
        for (uint i = 0; i < temperature.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_temp_data  << setprecision(9) << temperature[i] *epsilon_in_j/P_SI_KB << endl;
            // Process events
            process_events();
        }
        // Pressures * epsilon_in_j / (sigma_in_m * sigma_in_m * sigma_in_m) [Pa]
        for (uint i = 0; i < pressure.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_pressure_data<<setprecision(9)<< pressure[i]*epsilon_in_j/(sigma_in_m*sigma_in_m*sigma_in_m)<< endl;
            // Process events
            process_events();
        }
        // Unitless * 1
        for (uint i = 0; i < thermostat_values.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_therm_data << setprecision(9) << thermostat_values[i] << endl;
            // Process events
            process_events();
        }
        // Others
        for (uint i = 0; i < msd.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_msd_data   << setprecision(9) << msd[i]*sigma_in_m*sigma_in_m << endl;
            // Process events
            process_events();
        }
        for (uint i = 0; i < Cv.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_cv_data << setprecision(9) << Cv[i]*P_SI_KB/(1000 * particle_mass_in_kg) << endl; // [J/(g*K)]
            // Process events
            process_events();
        }
        for (uint i = 0; i < diffusion_coefficient.size(); i++) {
            if (abort_activities_requested) {
                break;
            }
            out_diff_c_data   << setprecision(9) << diffusion_coefficient[i]*sigma_in_m*sigma_in_m/sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j) << endl;
            // Process events
            process_events();
        }

        /////////Finish writing files///////////////////////////////////////////////////////

        out_etot_data .close();
        out_ep_data   .close();
        out_ek_data   .close();
        out_cv_data   .close();
        out_temp_data .close();
        out_therm_data.close();
        out_msd_data  .close();
        out_cohe_data .close();
        out_pressure_data.close();
    }
    output << "Writing to output files done." << endl;
    print_output_and_process_events();

#if  PRINT_OUTPUT_TO_TEXT_BOX
    for (uint i = 0; i < temperature.size();i++) // TODO: NOTE! not all vectors are of the same size (depending on which filter that is used)! Temperature is filtered and is smaller than for example pressure if emils filter is used
    {
        if (abort_activities_requested) {
            goto operation_finished;
        }

        // Lengths * sigma_in_m/P_ANGSTROM [Angstrom]
        // Energies * epsilon_in_j/P_EV [eV]
        output<<"E_tot           [eV]     = "<<setprecision(9)<< (Ek[i] + (Ep[i]+Ep_shift))*epsilon_in_j/P_SI_EV << endl;
        output<<"Ek              [eV]     = "<<setprecision(9)<< Ek[i]                     *epsilon_in_j/P_SI_EV << endl;
        output<<"Ep              [eV]     = "<<setprecision(9)<< (Ep[i]+Ep_shift)          *epsilon_in_j/P_SI_EV << endl;
        output<<"Cohesive energy [eV]     = "<<setprecision(9)<< cohesive_energy[i]        *epsilon_in_j/P_SI_EV << endl;
        // Masses * particle_mass_in_kg [kg]
        // Times * sqrt(particle_mass_in_kg * sigma_in_m * sigma_in_m / epsilon_in_j) [s]
        // Temperatures * epsilon_in_j/P_KB [K]
        output<<"Temp            [K]      = "<<setprecision(9)<< temperature[i] *epsilon_in_j/P_SI_KB <<endl;
        // Pressures * epsilon_in_j / (sigma_in_m * sigma_in_m * sigma_in_m) [Pa]
        output<<"Pressure        [Pa]     = "<<setprecision(9)<< pressure[i]*epsilon_in_j/(sigma_in_m*sigma_in_m*sigma_in_m) << endl;
        // Unitless * 1
        // Others
        output<<"Cv              [J/(gK)] = "<<setprecision(9)<< Cv[i] * P_SI_KB/(1000 * particle_mass_in_kg) << endl;
        output<<"msd             [m^2]    = "<<setprecision(9)<< msd[i] * sigma_in_m*sigma_in_m << endl;

        // Process events
        print_output_and_process_events();
    }
#endif

    Cv_sum = Cv_num = 0;
    for(uint i = uint(Cv.size()/6); i < Cv.size();i++) {
        if (abort_activities_requested) {
            goto operation_finished;
        }
        Cv_sum += Cv[i]*P_SI_KB/(1000 * particle_mass_in_kg);
        Cv_num++;
    }
    output << "*******************"<<endl;
    output << "Cv = "<< Cv_sum/Cv_num <<endl;
    output << "a=" << lattice_constant<<endl;
    output << "boxsize=" << box_size<<endl;
    output << "dt="<< dt << endl;
    output << "init_temp= "<<init_temp<<endl;
    output << "Complete" << endl;

operation_finished:
    // Finish the operation
    finish_operation();
}

void mdsystem::abort_activities()
{
    /*
     * This is not an *operation* in that sence since the variable being
     * changed cannot be locked for writing to a single thread
     */
    abort_activities_requested = true;
}

bool mdsystem::is_initialized() const
{
    return system_initialized;
}

bool mdsystem::is_operating() const
{
    return operating;
}

uint mdsystem::get_loop_num() const
{
    return loop_num;
}

uint mdsystem::get_max_loops_num() const
{
    return num_time_steps;
}

////////////////////////////////////////////////////////////////
// PRIVATE FUNCTIONS
////////////////////////////////////////////////////////////////

void mdsystem::init_particles() {
    // Allocate space for particles
    particles.resize(num_particles);

    //Place out particles according to the lattice pattern
    if (lattice_type == LT_FCC) {
        for (uint z = 0; z < box_size_in_lattice_constants; z++) {
            for (uint y = 0; y < box_size_in_lattice_constants; y++) {
                for (uint x = 0; x < box_size_in_lattice_constants; x++) {
                    int help_index = 4*(x + box_size_in_lattice_constants*(y + box_size_in_lattice_constants*z));

                    (particles[help_index + 0]).pos[0] = x*lattice_constant;
                    (particles[help_index + 0]).pos[1] = y*lattice_constant;
                    (particles[help_index + 0]).pos[2] = z*lattice_constant;

                    (particles[help_index + 1]).pos[0] = x*lattice_constant;
                    (particles[help_index + 1]).pos[1] = (y + ftype(0.5))*lattice_constant;
                    (particles[help_index + 1]).pos[2] = (z + ftype(0.5))*lattice_constant;

                    (particles[help_index + 2]).pos[0] = (x + ftype(0.5))*lattice_constant;
                    (particles[help_index + 2]).pos[1] = y*lattice_constant;
                    (particles[help_index + 2]).pos[2] = (z + ftype(0.5))*lattice_constant;

                    (particles[help_index + 3]).pos[0] = (x + ftype(0.5))*lattice_constant;
                    (particles[help_index + 3]).pos[1] = (y + ftype(0.5))*lattice_constant;
                    (particles[help_index + 3]).pos[2] = z*lattice_constant;
                } // X
            } // Y
        } // Z
    }
    
    //Randomize the velocities
    vec3 sum_vel = vec3(0, 0, 0);
    ftype sum_sqr_vel = 0;
    for (uint i = 0; i < num_particles; i++) {
        for (uint j = 0; j < 3; j++) {
            particles[i].vel[j] = 0;
            for (uint terms = 0; terms < 5; terms++) { //This will effectivelly create a distribution very similar to normal distribution. (If you want to see what the distribution looks like, go to www.wolframalpha.com/input/?i=fourier((sinc(x))^n) and replace n by the number of terms)
                particles[i].vel[j] += ftype(rand());
            }
        }
        sum_vel     += particles[i].vel;
        sum_sqr_vel += particles[i].vel.sqr_length();
    }

    // Compensate for incorrect start temperature and total velocities and finalize the initialization values
    vec3 average_vel = sum_vel/ftype(num_particles);
    ftype vel_variance = sum_sqr_vel/num_particles - average_vel.sqr_length();
    ftype scale_factor = sqrt(ftype(3.0)  * init_temp  / (vel_variance)); // Termal energy = 1.5 * P_KB * init_temp = 0.5 m v*v
    for (uint i = 0; i < num_particles; i++) {
        particles[i].vel = (particles[i].vel - average_vel)* scale_factor;
    }

    reset_non_modulated_relative_particle_positions();
}

void mdsystem::calculate_potential_energy_cutoff()
{
    ftype q;
    q = 1/sqr_inner_cutoff;
    q = q * q * q;
    E_cutoff = ftype(4.0) * q * (q - ftype(1.0));
}

void mdsystem::update_positions(ftype time_step)
{
    for (uint i = 0; i < num_particles; i++) {
        particles[i].pos += time_step * particles[i].vel;
        modulus_position(particles[i].pos);
    }
    update_verlet_list_if_necessary();
}

void mdsystem::update_velocities(ftype time_step)
{
    for (uint i = 0; i < num_particles; i++) {
        particles[i].vel += time_step * particles[i].acc;
    }
}

void mdsystem::update_verlet_list_if_necessary()
{
    // Check if largest displacement too large for not updating the Verlet list
    ftype sqr_limit = (sqr_outer_cutoff + sqr_inner_cutoff - 2*sqrt(sqr_outer_cutoff*sqr_inner_cutoff));
    uint i;
    // Check if any particle has move to much
    for (i = 0; i < num_particles; i++) {
        ftype sqr_displacement = origin_centered_modulus_position_minus(particles[i].pos, particles[i].pos_when_verlet_list_created).sqr_length();
        if (sqr_displacement > sqr_limit) {
            break;
        }
    }
    if (i < num_particles) {
        // Displacement that is to large was found
        output << "Verlet list updated. Simulation " << 100*loop_num/num_time_steps << " % done" <<endl;
        create_verlet_list();
    }
}

void mdsystem::create_verlet_list()
{
    bool         cells_used;            // Flag to tell is the cell list is used or not
    uint         box_size_in_cells;     // Given in one dimension TODO: Change name?
    ftype        cell_size;             // Could be the same as outer_cutoff but perhaps we should think about that...
    vector<uint> cell_linklist;         // Contains the particle index of the next particle (with decreasing order of the particles) that is in the same cell as the particle the list entry corresponds to. If these is no more particle in the cell, the entry will be 0.
    vector<uint> cell_list;             // Contains the largest particle index each cell contains. The list is coded as if each cell would contain particle zero (although it is probably not located there!)

    // Updating pos_when_verlet_list_created and non_modulated_relative_pos for all particles
    for (uint i = 0; i < num_particles; i++) {
        update_single_non_modulated_relative_particle_position(i);
        particles[i].pos_when_verlet_list_created = particles[i].pos;
    }

    // Check if the cells should be used for creating the Verlet list
    box_size_in_cells = uint(box_size/outer_cutoff);
    if (box_size_in_cells > 3) {
        // Cells will be used
        cells_used = true;
        cell_size = box_size/box_size_in_cells;
        create_linked_cells(box_size_in_cells, cell_size, cell_linklist, cell_list);
    }
    else {
        cells_used = false;
        cell_size = 0; // Not used (make warning shut-up)
    }

    //Creating new verlet_list
    uint cellindex = 0;
    uint neighbour_particle_index = 0;
    verlet_particles_list.resize(num_particles);
    verlet_particles_list[0] = 0;
    verlet_neighbors_list.resize(0); //The elements will be push_back'ed to the Verlet list
    for (uint i = 0; i < num_particles;) { // Loop through all particles
        // Init this neighbour list and point to the next list
        verlet_neighbors_list.push_back(0); // Reset number of neighbours
        int next_particle_list = verlet_particles_list[i] + 1; // Link to the next particle list

        if (cells_used) { //Loop through all neighbour cells
            // Calculate cell indexes
            uint cellindex_x = int(particles[i].pos[0]/cell_size);
            uint cellindex_y = int(particles[i].pos[1]/cell_size);
            uint cellindex_z = int(particles[i].pos[2]/cell_size);
            if (cellindex_x == box_size_in_cells || cellindex_y == box_size_in_cells || cellindex_z == box_size_in_cells) { // This actually occationally happens
                cellindex_x -= cellindex_x == box_size_in_cells;
                cellindex_y -= cellindex_y == box_size_in_cells;
                cellindex_z -= cellindex_z == box_size_in_cells;
            }
            for (int index_z = int(cellindex_z) - 1; index_z <= int(cellindex_z) + 1; index_z++) {
                for (int index_y = int(cellindex_y) - 1; index_y <= int(cellindex_y) + 1; index_y++) {
                    for (int index_x = int(cellindex_x) - 1; index_x <= int(cellindex_x) + 1; index_x++) {
                        int modulated_x = index_x;
                        int modulated_y = index_y;
                        int modulated_z = index_z;
                        // Control boundaries
                        if (modulated_x == -1) {
                            modulated_x = int(box_size_in_cells) - 1;
                        }
                        else if (modulated_x == int(box_size_in_cells)) {
                            modulated_x = 0;
                        }
                        if (modulated_y == -1) {
                            modulated_y = int(box_size_in_cells) - 1;
                        }
                        else if (modulated_y == int(box_size_in_cells)) {
                            modulated_y = 0;
                        }
                        if (modulated_z == -1) {
                            modulated_z = int(box_size_in_cells) - 1;
                        }
                        else if (modulated_z == int(box_size_in_cells)) {
                            modulated_z = 0;
                        }
                        cellindex = uint(modulated_x + box_size_in_cells * (modulated_y + box_size_in_cells * modulated_z)); // Calculate neighbouring cell index
                        neighbour_particle_index = cell_list[cellindex]; // Get the largest particle index of the particles in this cell
                        while (neighbour_particle_index > i) { // Loop though all particles in the cell with greater index
                            // TODO: The modulus can be removed if
                            ftype sqr_distance = origin_centered_modulus_position_minus(particles[i].pos, particles[neighbour_particle_index].pos).sqr_length();
                            if(sqr_distance < sqr_outer_cutoff) {
                                verlet_neighbors_list[verlet_particles_list[i]] += 1;
                                verlet_neighbors_list.push_back(neighbour_particle_index);
                                next_particle_list++;
                            }
                            neighbour_particle_index = cell_linklist[neighbour_particle_index]; // Get the next particle in the cell
                        }
                    } // X
                } // Y
            } // Z
        } // if (cells_used)
        else {
            for (neighbour_particle_index = i+1; neighbour_particle_index < num_particles; neighbour_particle_index++) { // Loop though all particles with greater index
                ftype sqr_distance = origin_centered_modulus_position_minus(particles[i].pos, particles[neighbour_particle_index].pos).sqr_length();
                if(sqr_distance < sqr_outer_cutoff) {
                    verlet_neighbors_list[verlet_particles_list[i]] += 1;
                    verlet_neighbors_list.push_back(neighbour_particle_index);
                    next_particle_list++;
                }
            }
        }
        i++; // Continue with the next particle (if there exists any)
        if (i < num_particles) { // Point to the next particle list
            verlet_particles_list[i] = next_particle_list;
        }
    }
}

void mdsystem::create_linked_cells(uint box_size_in_cells, ftype cell_size, vector<uint> &cell_linklist, vector<uint> &cell_list) {//Assuming origo in the corner of the bulk, and positions given according to boundaryconditions i.e. between zero and lenght of the bulk.
    int cellindex = 0;
    cell_list.resize(box_size_in_cells*box_size_in_cells*box_size_in_cells);
    cell_linklist.resize(num_particles);
    for (uint i = 0; i < cell_list.size() ; i++) {
        cell_list[i] = 0; // Beware! Particle zero is a member of all cells!
    }
    for (uint i = 0; i < num_particles; i++) {
        uint help_x = int(particles[i].pos[0] / cell_size);
        uint help_y = int(particles[i].pos[1] / cell_size);
        uint help_z = int(particles[i].pos[2] / cell_size);
        if (help_x == box_size_in_cells || help_y == box_size_in_cells || help_z == box_size_in_cells) { // This actually occationally happens
            help_x -= help_x == box_size_in_cells;
            help_y -= help_y == box_size_in_cells;
            help_z -= help_z == box_size_in_cells;
        }
        cellindex = help_x + box_size_in_cells * (help_y + box_size_in_cells * help_z);
        cell_linklist[i] = cell_list[cellindex];
        cell_list[cellindex] = i;
    }
}

void mdsystem::reset_non_modulated_relative_particle_positions()
{
    for (uint i = 0; i < num_particles; i++) {
        reset_single_non_modulated_relative_particle_positions(i);
    }
}

inline void mdsystem::reset_single_non_modulated_relative_particle_positions(uint i)
{
    particles[i].non_modulated_relative_pos = vec3(0, 0, 0);
    particles[i].pos_when_non_modulated_relative_pos_was_calculated = particles[i].pos;
}

void mdsystem::update_non_modulated_relative_particle_positions()
{
    for (uint i = 0; i < num_particles; i++) {
        update_single_non_modulated_relative_particle_position(i);
    }
}

inline void mdsystem::update_single_non_modulated_relative_particle_position(uint i)
{
    particles[i].non_modulated_relative_pos += origin_centered_modulus_position_minus(particles[i].pos, particles[i].pos_when_non_modulated_relative_pos_was_calculated);
    particles[i].pos_when_non_modulated_relative_pos_was_calculated = particles[i].pos;
}

void mdsystem::enter_loop_number(uint loop_to_enter)
{
    loop_num = loop_to_enter;
    sampling_in_this_loop = !(loop_num % sampling_period);
    current_sample_index = loop_num / sampling_period;
}

void mdsystem::enter_next_loop()
{
    enter_loop_number(loop_num + 1);
}

void mdsystem::leapfrog()
{
    /*
     * The velocities are supposed to be half a time step behind all the time
     * except from when properties are going to be measured or just have been
     * measured.
     */

    // Update velocities
    if (sampling_in_this_loop) { // Only take half the time step
        update_velocities(dt/2);
    }
    else {
#if THERMOSTAT == CHING_CHIS_THERMOSTAT
        if (thermostat_on) { // Accelerate particles because of therometer
            for (uint i = 0; i < num_particles; i++) {
                particles[i].vel = particles[i].vel * thermostat_value;
            }
        }
#endif
        update_velocities(dt);
    }

    // Update positions
    update_positions(dt);

    /*
     * Now the particle has updated both the velocity and the position, so it is
     * time to enter the next loop.
     */
    enter_next_loop();

    // Update velocities again if needed
    if (sampling_in_this_loop) {
        // Calculate the forces in the new positions and then do the usual routine
        calculate_forces();
#if THERMOSTAT == CHING_CHIS_THERMOSTAT
        // Accelerate particles because of therometer
        if (thermostat_on) {
            for (uint i = 0; i < num_particles; i++) {
                particles[i].vel = particles[i].vel * thermostat_value;
            }
        }
#endif
        // Take a half timestep to let the position "catch up" with the velocity
        update_velocities(dt/2);

        // Also measure unfiltered properties
        measure_unfiltered_properties();
    }
}

void mdsystem::calculate_forces()
{
    // Reset accelrations for all particles
    for (uint k = 0; k < num_particles; k++) {
        particles[k].acc = vec3(0, 0, 0);
    }
    if (sampling_in_this_loop) {
        instEp[current_sample_index] = 0;
        distance_force_sum[current_sample_index] = 0;
    }

    for (uint i1 = 0; i1 < num_particles ; i1++) { // Loop through all particles
        for (uint j = verlet_particles_list[i1] + 1; j < verlet_particles_list[i1] + verlet_neighbors_list[verlet_particles_list[i1]] + 1 ; j++) {
            // TODO: automatically detect if a boundary is crossed and compensate for that in this function
            // Calculate the closest distance to the second (possibly) interacting particle
            uint i2 = verlet_neighbors_list[j];
            vec3 r = origin_centered_modulus_position_minus(particles[i1].pos, particles[i2].pos);
            ftype sqr_distance = r.sqr_length();
            if (sqr_distance >= sqr_inner_cutoff) {
                continue; // Skip this interaction and continue with the next one
            }
            ftype sqr_distance_inv = 1/sqr_distance;
            ftype distance_inv = sqrt(sqr_distance_inv);

            //Calculating acceleration
            ftype p = sqr_distance_inv;
            p = p*p*p;
            ftype acceleration = 48  * distance_inv * p * (p - ftype(0.5));

            // Update accelerations of interacting particles
            vec3 r_hat = r * distance_inv;
            particles[i1].acc +=  acceleration * r_hat;
            particles[i2].acc -=  acceleration * r_hat;

            // Update properties
            //TODO: Remove these two from force calculation and place them somewhere else
            if (sampling_in_this_loop) {
                if (Ep_on      ) instEp[current_sample_index] += 4 * p * (p - 1) - E_cutoff;
                if (pressure_on) distance_force_sum[current_sample_index] += acceleration / distance_inv;
            }
        }
    }
    //TODO: Move this from here, since it's filtered anyway (Right?)
    if (sampling_in_this_loop && Ep_on) {
        instEc[current_sample_index] = -instEp[current_sample_index]/num_particles;
    }

#if THERMOSTAT == LASSES_THERMOSTAT
    // Add acceleration caused by the thermostat
    if (thermostat_on) {
        for (uint i = 0; i < num_particles; i++) {
            particles[i].acc -= thermostat_value * particles[i].vel;
        }
    }
#endif
}

void mdsystem::measure_unfiltered_properties() {
    /*
     * This functions assumes that fource_calculation() has just been called for
     * the current positions
     */
    // Update relative positions
    update_non_modulated_relative_particle_positions();

    // Calculate the sumn of the square velcities
    ftype sum_sqr_vel = 0;
    for (uint i = 0; i < num_particles; i++) {
        sum_sqr_vel = sum_sqr_vel + particles[i].vel.sqr_length();
    }

    // Take the samples and do the measurementas
    insttemp[current_sample_index] =  sum_sqr_vel / (3 * num_particles);
    if (Ek_on) instEk[current_sample_index] = 0.5f * sum_sqr_vel;

    calculate_thermostate_value();

    if (msd_on   ) calculate_mean_square_displacement();
    if (diff_c_on) calculate_diffusion_coefficient   ();
}

void mdsystem::calculate_thermostate_value()
{
#if THERMOSTAT == LASSES_THERMOSTAT // Additive
    const ftype thermostat_value_when_extreme_cooling = 1/dt;
    const ftype thermostat_value_when_inactive = 0;
#elif THERMOSTAT == CHING_CHIS_THERMOSTAT // Multiplicative
    const ftype thermostat_value_when_extreme_cooling = 0;
    const ftype thermostat_value_when_inactive = 1;
#endif

    if (thermostat_on && insttemp[current_sample_index] > 0) {
#if THERMOSTAT == LASSES_THERMOSTAT
        thermostat_value = (1 - desired_temp/insttemp[current_sample_index]) / (2*thermostat_time);
        thermostat_value = thermostat_value < 1/dt ? thermostat_value : 1/dt;
#elif THERMOSTAT == CHING_CHIS_THERMOSTAT
        ftype arg = 1 +  dt / thermostat_time * (desired_temp / insttemp[current_sample_index] - 1);
        thermostat_value = arg > 0                 ? sqrt(arg)        : 0   ;
#endif
        if (thermostat_value != thermostat_value_when_extreme_cooling) {
            if (current_sample_index != 0 && thermostat_values[current_sample_index-1] == thermostat_value_when_extreme_cooling) {
                output << "Thermostat can relax a bit. " << 100*loop_num/num_time_steps << " % done." << endl;
            }
        }
        else if (current_sample_index == 0 || thermostat_values[current_sample_index-1] != thermostat_value_when_extreme_cooling) {
            output << "Thermostat working at maximum to cool the system. Simulation " << 100*loop_num/num_time_steps << " % done." << endl;
        }
    }
    else {
        thermostat_value = thermostat_value_when_inactive;
        if (thermostat_on) {
            if (current_sample_index == 0) {
                output << "Thermostat does not function at 0 K" << endl;
            }
            else if (insttemp[0] > 0 && insttemp[current_sample_index-1] > 0) {
                output << "Zero Kelvin reached. " << 100*loop_num/num_time_steps << " % done." << endl;
            }
        }
    }

    // Store thermostat value
    thermostat_values[current_sample_index] = thermostat_value;
}

void mdsystem::calculate_filtered_properties()
{
    filter(insttemp, temperature, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
    if (Cv_on) calculate_specific_heat();            
    if (pressure_on) calculate_pressure();
    if (Ep_on) {
        filter(instEp, Ep             , default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
        filter(instEc, cohesive_energy, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
    }
    if (Ek_on) filter(instEk, Ek, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);
}

void mdsystem::calculate_specific_heat() {
    ftype impulse_response_decay_time = ftype(2000)*P_RU_FS;
    ftype num_times_filtering = 1;
    bool  slope_compensate = false;
    vector<ftype> filtered_temp;
#if 0
    vector<ftype> unfiltered_temp2(insttemp.size());
    vector<ftype> filtered_temp2;

    // Calculate local variance of insttemp
    for (uint i = 0; i < insttemp.size(); i++){
        unfiltered_temp2[i] = insttemp[i]*insttemp[i];
    }
    filter(insttemp        , filtered_temp , impulse_response_decay_time, num_times_filtering);
    filter(unfiltered_temp2, filtered_temp2, impulse_response_decay_time, num_times_filtering);

    // Calculate Cv
    Cv.resize(filtered_temp.size());
    for (uint i = 0; i < Cv.size(); i++) {
        Cv[i] = ftype(1.0)/(ftype(2.0/3.0) - num_particles*(filtered_temp2[i]/(filtered_temp[i]*filtered_temp[i]) - 1));
    }
#else
    vector<ftype> unfiltered_var(insttemp.size());
    vector<ftype> filtered_var;

    // Calculate local variance of insttemp
    filter(insttemp, filtered_temp, impulse_response_decay_time, num_times_filtering, slope_compensate);
    for (uint i = 0; i < insttemp.size(); i++){
        //unfiltered_var[i] = insttemp[i]*insttemp[i] - filtered_temp[i]*filtered_temp[i];
        unfiltered_var[i] = (insttemp[i] - filtered_temp[i])*(insttemp[i] - filtered_temp[i]);
    }
    filter(unfiltered_var, filtered_var, impulse_response_decay_time, num_times_filtering, false);

    // Calculate Cv
    Cv.resize(filtered_temp.size());
    for (uint i = 0; i < Cv.size(); i++) {
        Cv[i] = ftype(1.0)/(ftype(2.0/3.0) - num_particles*filtered_var[i]/(filtered_temp[i]*filtered_temp[i]));
    }
#endif
}

void mdsystem::calculate_pressure() {
    ftype V = box_size*box_size*box_size;
    vector<ftype> filtered_distance_force_sum;
    filter(distance_force_sum, filtered_distance_force_sum, default_impulse_response_decay_time, default_num_times_filtering, slope_compensate_by_default);

    pressure.resize(filtered_distance_force_sum.size());
    for (uint i = 0; i < pressure.size(); i++) {
        pressure[i] = num_particles*temperature[i]/V + filtered_distance_force_sum[i]/(3*V);
    }
}

void mdsystem::calculate_mean_square_displacement() {
    ftype sum = 0;
    if (!equilibrium_reached) {
        // Equilibrium has not previously been reached; don't calculate this property.
        msd[current_sample_index] = 0;
        // Check if equilibrium has been reached
        if (current_sample_index >= 1) {
            ftype variation = (instEp[current_sample_index] - instEp[current_sample_index - 1]) / instEp[current_sample_index];
            variation = variation >= 0 ? variation : -variation;
            if (variation < dEp_tolerance) { //TODO: Is this a sufficient check? Probably not
                sample_index_when_equilibrium_reached = current_sample_index;
                equilibrium_reached = true; // The requirements for equilibrium has been reached
                reset_non_modulated_relative_particle_positions(); // Consider the particles to "start" now
            }
        }
    }
    else {
        // Equilibrium has previously been reached
        // Calculate mean square displacement
        for (uint i = 0; i < num_particles;i++) {
            sum += particles[i].non_modulated_relative_pos.sqr_length();
        }
        sum = sum/num_particles;
        msd[current_sample_index] = sum;
    }
}

void mdsystem::calculate_diffusion_coefficient()
{
    if (equilibrium_reached && current_sample_index > sample_index_when_equilibrium_reached) {
        diffusion_coefficient[current_sample_index] = msd[current_sample_index]/(6*dt*sampling_period*(current_sample_index - sample_index_when_equilibrium_reached));
    }
    else {
        diffusion_coefficient[current_sample_index] = 0;
    }
}

void mdsystem::filter(const vector<ftype> &unfiltered, vector<ftype> &filtered, ftype impulse_response_decay_time, uint num_times, bool slope_compensate)
{
#if FILTER == KRISTOFERS_FILTER
    if (num_times < 0) {
        throw invalid_argument("Negative number of times filtering");
    }

    uint vector_size = unfiltered.size();
    filtered.resize(vector_size);

    if (num_times == 0) {
        for (uint i = 0; i < vector_size; i++) {
            filtered[i] = unfiltered[i];
        }
        return;
    }

    ftype f = exp(-dt*sampling_period/impulse_response_decay_time);
    ftype k = 1 - f;
    ftype x, y, w;
    vector<ftype> total_weight(vector_size);
    const vector<ftype> *source;
    vector<ftype> *destination;
    vector<ftype> temp_vec(num_times > 1 ? vector_size : 0);
    vector<ftype> filtered_index(vector_size);

    source = &unfiltered;
    for (; num_times > 0; num_times--) {
        destination = (num_times & 1) ? &filtered : &temp_vec;
        // Left side exponential decay
        x = y = w = 0;
        for (int i = 0; i < int(vector_size); i++) {
            x *= f;
            y *= f;
            w *= f;
            x += k*i           ;
            y += k*(*source)[i];
            w += k             ;
            filtered_index[i] = x;
            (*destination)[i] = y;
            total_weight  [i] = w;
        }

        // Right side exponential decay
        x = y = w = 0;
        for (int i = int(vector_size) - 1; i >= 0; i--) {
            x *= f;
            y *= f;
            w *= f;
            filtered_index[i] += x;
            (*destination)[i] += y;
            total_weight  [i] += w;
            x += k*i           ;
            y += k*(*source)[i];
            w += k             ;

            // Compensate for weights at the same time
            (*destination)[i] /= total_weight[i];
            filtered_index[i] /= total_weight[i];
        }

        if (slope_compensate) {
            vector<ftype> dy_dx(vector_size);
            dy_dx[0] = (4*((*destination)[1]-(*destination)[0])+(*destination)[0]-(*destination)[2])/
                       (4*(filtered_index[1]-filtered_index[0])+filtered_index[0]-filtered_index[2]);
            for (int i = 1; i < int(vector_size) - 1; i++) {
                dy_dx[i] = ((*destination)[i+1]-(*destination)[i-1])/
                        (filtered_index[i+1]-filtered_index[i-1]);
            }
            dy_dx[vector_size-1] = (4*((*destination)[vector_size-1]-(*destination)[vector_size-2])-(*destination)[vector_size-1]+(*destination)[vector_size-3])/
                                   (4*(filtered_index[vector_size-1]-filtered_index[vector_size-2])-filtered_index[vector_size-1]+filtered_index[vector_size-3]);
            for (int i = 0; i < int(vector_size); i++) {
                (*destination)[i] += (i-filtered_index[i])*dy_dx[i];
            }
        }

        source = destination;
    }
#elif FILTER == EMILS_FILTER
    filtered.resize(unfiltered.size()/ensemble_size);
    for(uint i = 0; i < filtered.size(); i++){
        ftype sum = 0;
        for(uint j = 0; j < ensemble_size; j++){
            sum += unfiltered[i*ensemble_size+j];
        }
        filtered[i] = sum / ensemble_size;
    }
#endif
}

ofstream* mdsystem::open_ofstream_file(ofstream &o, const char* path) const
{
    o.open(path);
    return &o;
}

void mdsystem::modulus_position(vec3 &pos) const
{
    // Check boundaries in x-direction
    if (pos[0] >= box_size) {
        pos[0] -= box_size;
        while (pos[0] >= box_size) {
            pos[0] -= box_size;
        }
    }
    else {
        while (pos[0] < 0) {
            pos[0] += box_size;
        }
    }

    // Check boundaries in y-direction
    if (pos[1] >= box_size) {
        pos[1] -= box_size;
        while (pos[1] >= box_size) {
            pos[1] -= box_size;
        }
    }
    else {
        while (pos[1] < 0) {
            pos[1] += box_size;
        }
    }

    // Check boundaries in z-direction
    if (pos[2] >= box_size) {
        pos[2] -= box_size;
        while (pos[2] >= box_size) {
            pos[2] -= box_size;
        }
    }
    else {
        while (pos[2] < 0) {
            pos[2] += box_size;
        }
    }
}

void mdsystem::origin_centered_modulus_position(vec3 &pos) const
{
    // Check boundaries in x-direction
    if (pos[0] >= pos_half_box_size) {
        pos[0] -= box_size;
        while (pos[0] >= pos_half_box_size) {
            pos[0] -= box_size;
        }
    }
    else {
        while (pos[0] < neg_half_box_size) {
            pos[0] += box_size;
        }
    }

    // Check boundaries in y-direction
    if (pos[1] >= pos_half_box_size) {
        pos[1] -= box_size;
        while (pos[1] >= pos_half_box_size) {
            pos[1] -= box_size;
        }
    }
    else {
        while (pos[1] < neg_half_box_size) {
            pos[1] += box_size;
        }
    }

    // Check boundaries in z-direction
    if (pos[2] >= pos_half_box_size) {
        pos[2] -= box_size;
        while (pos[2] >= pos_half_box_size) {
            pos[2] -= box_size;
        }
    }
    else {
        while (pos[2] < neg_half_box_size) {
            pos[2] += box_size;
        }
    }
}

vec3 mdsystem::origin_centered_modulus_position_minus(vec3 pos1, vec3 pos2) const
{
    vec3 d = pos1 - pos2;
    origin_centered_modulus_position(d);
    return d;
}

void mdsystem::print_output_and_process_events()
{
    print_output();
    process_events();
}

void mdsystem::process_events()
{
    // Let the application process its events
    if (event_callback.func) {
        event_callback.func(event_callback.param);
    }
}

void mdsystem::print_output()
{
    if (output.str().empty()) {
        // Nothing to write
        return;
    }

    // Print the contents of the output buffer and then empty it
    if (output_callback.func) {
        output_callback.func(output_callback.param, output.str());
    }
    output.str("");
}

void mdsystem::start_operation()
{
    while (operating) {
        // Wait for the other operation to finish
        process_events();
    }
    operating = true;
}

void mdsystem::finish_operation()
{
    print_output();
    if (!operating) {
        throw runtime_error("Tried to finish operation that was never started");
    }
    operating = false;
}
