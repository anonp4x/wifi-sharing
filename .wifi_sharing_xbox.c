#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>

// Global variables for GUI components
GtkWidget *wifi_combo;
GtkWidget *eth_combo;
GtkWidget *status_label;
GtkWidget *upnp_checkbox;

// Function to run system commands and print output
void run_command(const char *command) {
    int result = system(command);
    if (result != 0) {
        fprintf(stderr, "Error running command: %s\n", command);
    }
}

// Function to check for port 53 conflicts
int check_port_conflict() {
    FILE *fp = popen("sudo lsof -i :53", "r");
    if (fp == NULL) {
        perror("Failed to run port check command");
        return 1;
    }
    
    char line[256];
    while (fgets(line, sizeof(line) - 1, fp) != NULL) {
        if (strstr(line, "systemd-resolved") != NULL) {
            pclose(fp);
            return 1;
        }
    }
    pclose(fp);
    return 0;
}

// Function to install and configure necessary packages
void install_and_configure() {
    // Install required packages
    run_command("sudo apt-get update");
    run_command("sudo apt-get install -y dnsmasq iptables miniupnpd");

    // Check for port conflicts
    if (check_port_conflict()) {
        gtk_label_set_text(GTK_LABEL(status_label), "Port 53 conflict detected. Please resolve it before proceeding.");
        return;
    }

    // Configure dnsmasq
    FILE *dnsmasq_conf = fopen("/etc/dnsmasq.conf", "w");
    if (dnsmasq_conf) {
        fprintf(dnsmasq_conf, "interface=eth0\n");
        fprintf(dnsmasq_conf, "dhcp-range=192.168.137.2,192.168.137.50,255.255.255.0,12h\n");
        fclose(dnsmasq_conf);
        run_command("sudo dnsmasq --test");
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "Error creating dnsmasq configuration file.");
        return;
    }

    // Enable IP forwarding
    run_command("echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward");

    // Ensure firewall rules are in place
    run_command("sudo iptables -A INPUT -p udp --dport 67:68 --sport 67:68 -j ACCEPT");
    run_command("sudo iptables -A INPUT -p tcp --dport 53 -j ACCEPT");
    run_command("sudo iptables -A INPUT -p udp --dport 53 -j ACCEPT");

    // Restart and enable services
    run_command("sudo systemctl restart dnsmasq");
    run_command("sudo systemctl enable dnsmasq");
    run_command("sudo systemctl restart miniupnpd");
    run_command("sudo systemctl enable miniupnpd");
}

// Function to populate network interfaces into combo boxes
void populate_interfaces(GtkWidget *combo, const char *iface_type) {
    char command[128];
    char line[128];
    FILE *fp;

    snprintf(command, sizeof(command), "ip link show | grep '%s' | awk '{print $2}' | tr -d ':'", iface_type);
    fp = popen(command, "r");

    if (fp == NULL) {
        perror("Failed to run command");
        return;
    }

    while (fgets(line, sizeof(line)-1, fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';  // Remove newline character
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), line);
    }

    pclose(fp);
}

// Function to start internet sharing
static void start_sharing(GtkWidget *widget, gpointer data) {
    const char *wifi_iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(wifi_combo));
    const char *eth_iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(eth_combo));
    gboolean enable_upnp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(upnp_checkbox));

    if (!wifi_iface || !eth_iface) {
        gtk_label_set_text(GTK_LABEL(status_label), "Please select both interfaces.");
        return;
    }

    char command[256];

    // Configure iptables for NAT
    snprintf(command, sizeof(command), "iptables -t nat -A POSTROUTING -o %s -j MASQUERADE", wifi_iface);
    run_command(command);

    snprintf(command, sizeof(command), "iptables -A FORWARD -i %s -o %s -m state --state RELATED,ESTABLISHED -j ACCEPT", wifi_iface, eth_iface);
    run_command(command);

    snprintf(command, sizeof(command), "iptables -A FORWARD -i %s -o %s -j ACCEPT", eth_iface, wifi_iface);
    run_command(command);

    // Start dnsmasq for DHCP server
    run_command("sudo systemctl start dnsmasq");

    // Optionally start UPnP for Xbox multiplayer and voice chat support
    if (enable_upnp) {
        run_command("sudo systemctl start miniupnpd");
        gtk_label_set_text(GTK_LABEL(status_label), "Internet sharing started with UPnP enabled.");
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "Internet sharing started without UPnP.");
    }
}

// Function to stop internet sharing
static void stop_sharing(GtkWidget *widget, gpointer data) {
    const char *wifi_iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(wifi_combo));
    const char *eth_iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(eth_combo));

    if (!wifi_iface || !eth_iface) {
        gtk_label_set_text(GTK_LABEL(status_label), "Please select both interfaces.");
        return;
    }

    char command[256];

    // Disable IP forwarding
    run_command("echo 0 > /proc/sys/net/ipv4/ip_forward");

    // Remove iptables rules
    snprintf(command, sizeof(command), "iptables -t nat -D POSTROUTING -o %s -j MASQUERADE", wifi_iface);
    run_command(command);

    snprintf(command, sizeof(command), "iptables -D FORWARD -i %s -o %s -m state --state RELATED,ESTABLISHED -j ACCEPT", wifi_iface, eth_iface);
    run_command(command);

    snprintf(command, sizeof(command), "iptables -D FORWARD -i %s -o %s -j ACCEPT", eth_iface, wifi_iface);
    run_command(command);

    // Stop dnsmasq
    run_command("sudo systemctl stop dnsmasq");

    // Stop UPnP
    run_command("sudo systemctl stop miniupnpd");

    gtk_label_set_text(GTK_LABEL(status_label), "Internet sharing stopped.");
}

// Main function to set up the graphical interface and event handling
int main(int argc, char *argv[]) {
    GtkWidget *window;
    GtkWidget *grid;
    GtkWidget *start_button;
    GtkWidget *stop_button;
    GtkWidget *wifi_label;
    GtkWidget *eth_label;
    GtkWidget *upnp_label;

    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Wi-Fi to Ethernet Sharing");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 200);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);

    // Install and configure packages
    install_and_configure();

    // Labels and combo boxes for network interfaces
    wifi_label = gtk_label_new("Select Wi-Fi Interface:");
    eth_label = gtk_label_new("Select Ethernet Interface:");
    gtk_grid_attach(GTK_GRID(grid), wifi_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), eth_label, 0, 1, 1, 1);

    wifi_combo = gtk_combo_box_text_new();
    populate_interfaces(wifi_combo, "wl");
    gtk_grid_attach(GTK_GRID(grid), wifi_combo, 1, 0, 2, 1);

    eth_combo = gtk_combo_box_text_new();
    populate_interfaces(eth_combo, "eth");
    gtk_grid_attach(GTK_GRID(grid), eth_combo, 1, 1, 2, 1);

    // UPnP checkbox
    upnp_checkbox = gtk_check_button_new_with_label("Enable UPnP (recommended for Xbox)");
    gtk_grid_attach(GTK_GRID(grid), upnp_checkbox, 0, 2, 3, 1);

    // Status label
    status_label = gtk_label_new("Status: Idle");
    gtk_grid_attach(GTK_GRID(grid), status_label, 0, 3, 3, 1);

    // Start and Stop buttons
    start_button = gtk_button_new_with_label("Start Sharing");
    g_signal_connect(start_button, "clicked", G_CALLBACK(start_sharing), NULL);
    gtk_grid_attach(GTK_GRID(grid), start_button, 0, 4, 1, 1);

    stop_button = gtk_button_new_with_label("Stop Sharing");
    g_signal_connect(stop_button, "clicked", G_CALLBACK(stop_sharing), NULL);
    gtk_grid_attach(GTK_GRID(grid), stop_button, 1, 4, 1, 1);

    gtk_widget_show_all(window);

    gtk_main();

    return 0;
}
