#include <acpi/acpi.h>
#include <acpispec/tables.h>
#include <klog/klog.h>
#include <serial/serial.h>
#include <lai/helpers/sci.h>
#include <lai/host.h>
#include <panic.h>
#include <stdint.h>
#include <string.h>
#include <term/term.h>
#include <mem/pmm.h>
#include <time/timer.h>
#include <debug/debug.h>
#include <acpi/madt.h>

#define LAI_ACPI_MODE_LEGACY_8259 0
#define LAI_ACPI_MODE_APIC 1
#define LAI_ACPI_MODE_SAPIC 2

static acpi_xsdt_t *xsdt = NULL;
static acpi_rsdt_t *rsdt = NULL;

void acpi_init(acpi_xsdp_t *xsdp_table)
{
    // store xsdp_table pointer in a (static) global variable,
    // allowing us to use it in other functions in this file
    klog("acpi", "ACPI Revision: %d", xsdp_table->revision);
    if (xsdp_table->revision < 2)
    {
        rsdt = (acpi_rsdt_t *)((uint64_t)xsdp_table->rsdt + HIGHER_HALF) ;
    }
    else
    {
        xsdt = (acpi_xsdt_t *)((uint64_t)xsdp_table->xsdt + HIGHER_HALF) ;
    }
    
    // this has to be called after ACPI has been detected, but before LAI starts up
    // we *should* split the ACPI/LAI initialisation and put init_timer in main,
    // but initialising it from here works for now.
    hpet_init();

    klog("acpi", "rsdt=%x xsdt=%x", rsdt, xsdt);
    lai_set_acpi_revision(xsdp_table->revision);
    lai_create_namespace();    
    klog("acpi", "Enabling ACPI mode in LAI");
    lai_enable_acpi(LAI_ACPI_MODE_APIC);
    klog("acpi", "ACPI mode enabled");

    klog("acpi", "Initializing MADT");
    madt_init();
    klog("acpi", "MADT initialized");
}

void *acpi_find_table_rsdt(char *sig, size_t idx);
void *acpi_find_table_xsdt(char *sig, size_t idx);

void *acpi_find_table(char *sig, size_t idx)
{
    void *result = NULL;
    if (xsdt != NULL)
    {
        result = acpi_find_table_xsdt(sig, idx);
    }
    else
    {
        result = acpi_find_table_rsdt(sig, idx);
    }
    return result;
}

void *acpi_find_table_xsdt(char *sig, size_t idx)
{
    int entries = (xsdt->header.length - sizeof(acpi_header_t)) / (uint32_t)8;
    klog("acpi", "Searching %d table entries for %s", entries, sig);
    for (int t = 0; t < entries; t++)
    {
        acpi_header_t *new_header = (void *)(xsdt->tables[t] + HIGHER_HALF);
        char new_sig[5] = {0};
        memcpy(new_sig, new_header->signature, 4);
        klog("acpi", "Found header with signature %s, length=%d", new_sig, new_header->length);
        if (memcmp(new_header->signature, sig, 4) == 0)
        {
            if (idx == 0)
            {
                klog("acpi", "Header located at %x", new_header);
                return (void*) new_header;
            }
            idx--;
        }
    }
    return 0;
}

void *acpi_find_table_rsdt(char *sig, size_t idx)
{
    panic("RSDT mode not implemented yet (reboot using UEFI)");
    int entries = (rsdt->header.length - sizeof(acpi_header_t) / 4);
    klog("acpi", "Searching %d table entries for %s", entries, sig);
    for (int t = 0; t < entries; t++)
    {
        acpi_header_t *new_header = (acpi_header_t *)(uint64_t)rsdt->tables[t];
        klog("acpi", "Found header with signature %x", new_header->signature);
        if (memcmp(new_header->signature, sig, 4) == 0)
        {
            if (idx == 0)
            {
                return new_header;
            }
            idx--;
        }
    }
    return 0;
}
