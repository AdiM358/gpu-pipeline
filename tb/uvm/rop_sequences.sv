// Sequences: the stimulus side of the verification plan.
//
//   rop_clear_seq         one clear (the driver waits for idle first: P1)
//   rop_random_seq        uniformly random on-screen fragments, optional gaps
//   rop_hazard_seq        "hazard storm": a few pixels hit back to back with
//                         depth biased toward ties, closer and farther (F1, F2)
//   rop_edge_seq          fragments biased onto screen edges and corners (F6)
//   rop_clear_draw_seq    clears interleaved with draw traffic (F5)
//   rop_fwd_directed_seq  directed forwarding sweep, distance 1..5 (F2)

class rop_base_seq extends uvm_sequence #(rop_item);
    `uvm_object_utils(rop_base_seq)

    function new(string name = "rop_base_seq");
        super.new(name);
    endfunction

    // Sends one fragment with fully specified position, depth and gap; the
    // colour stays random.
    task send_frag(int unsigned fx, int unsigned fy, bit [15:0] fz, int unsigned fgap = 0);
        rop_item t = rop_item::type_id::create("frag");
        start_item(t);
        if (!t.randomize() with { op == OP_FRAG; x == fx; y == fy; z == fz; gap == fgap; })
            `uvm_fatal("RAND", "fragment randomization failed")
        finish_item(t);
    endtask

    // 25% of fragments get 1..max_gap idle cycles in front of them.
    function int unsigned pick_gap(int unsigned max_gap);
        if (max_gap == 0 || $urandom_range(0, 3) != 0) return 0;
        return $urandom_range(1, max_gap);
    endfunction
endclass

class rop_clear_seq extends rop_base_seq;
    rand bit [15:0] color;
    `uvm_object_utils(rop_clear_seq)

    function new(string name = "rop_clear_seq");
        super.new(name);
    endfunction

    task body();
        rop_item t = rop_item::type_id::create("clear");
        start_item(t);
        if (!t.randomize() with { op == OP_CLEAR; clear_color == local::color; })
            `uvm_fatal("RAND", "clear randomization failed")
        finish_item(t);
    endtask
endclass

class rop_random_seq extends rop_base_seq;
    int unsigned n_frags = 1000;
    int unsigned max_gap = 0;
    `uvm_object_utils(rop_random_seq)

    function new(string name = "rop_random_seq");
        super.new(name);
    endfunction

    task body();
        repeat (n_frags) begin
            rop_item t = rop_item::type_id::create("frag");
            int unsigned g = pick_gap(max_gap);
            start_item(t);
            if (!t.randomize() with { op == OP_FRAG; gap == g; })
                `uvm_fatal("RAND", "fragment randomization failed")
            finish_item(t);
        end
    endtask
endclass

class rop_hazard_seq extends rop_base_seq;
    int unsigned n_frags = 2000;
    int unsigned pool    = 4;    // distinct pixels in play: small, so hits repeat
    int unsigned max_gap = 4;    // idle cycles up to this, to cross distances 1..5
    `uvm_object_utils(rop_hazard_seq)

    function new(string name = "rop_hazard_seq");
        super.new(name);
    endfunction

    task body();
        int unsigned px[$], py[$];
        bit [15:0]   last_z[int];   // last depth *sent* to each pool pixel
        repeat (pool) begin
            px.push_back($urandom_range(0, SCREEN_W - 1));
            py.push_back($urandom_range(0, SCREEN_H - 1));
        end
        repeat (n_frags) begin
            int unsigned k   = $urandom_range(0, pool - 1);
            int unsigned sel = $urandom_range(0, 99);
            int          base = last_z.exists(k) ? last_z[k] : 16'h8000;
            int          zv;
            if      (sel < 25) zv = base;                                // tie if the last one passed
            else if (sel < 50) zv = base - int'($urandom_range(1, 64));  // closer: runs of writes
            else if (sel < 70) zv = base + int'($urandom_range(1, 64));  // farther
            else if (sel < 77) zv = 0;
            else if (sel < 84) zv = 16'hFFFF;
            else               zv = $urandom_range(0, 16'hFFFF);
            if (zv < 0)       zv = 0;
            if (zv > 16'hFFFF) zv = 16'hFFFF;
            send_frag(px[k], py[k], 16'(zv), pick_gap(max_gap));
            last_z[k] = 16'(zv);
        end
    endtask
endclass

class rop_edge_seq extends rop_base_seq;
    int unsigned n_frags = 600;
    `uvm_object_utils(rop_edge_seq)

    function new(string name = "rop_edge_seq");
        super.new(name);
    endfunction

    task body();
        repeat (n_frags) begin
            int unsigned sx = $urandom_range(0, 2), sy = $urandom_range(0, 2);
            int unsigned fx = (sx == 0) ? 0 : (sx == 1) ? SCREEN_W - 1 : $urandom_range(1, SCREEN_W - 2);
            int unsigned fy = (sy == 0) ? 0 : (sy == 1) ? SCREEN_H - 1 : $urandom_range(1, SCREEN_H - 2);
            send_frag(fx, fy, 16'($urandom_range(0, 16'hFFFF)));
        end
    endtask
endclass

class rop_clear_draw_seq extends rop_base_seq;
    int unsigned rounds          = 3;
    int unsigned frags_per_round = 400;
    `uvm_object_utils(rop_clear_draw_seq)

    function new(string name = "rop_clear_draw_seq");
        super.new(name);
    endfunction

    task body();
        for (int i = 0; i < rounds; i++) begin
            rop_clear_seq  clr = rop_clear_seq::type_id::create("clr");
            rop_hazard_seq hz  = rop_hazard_seq::type_id::create("hz");
            // Cycle through black, white and a random colour.
            clr.color = (i % 3 == 0) ? 16'h0000 : (i % 3 == 1) ? 16'hFFFF : 16'($urandom);
            clr.start(m_sequencer, this);
            hz.n_frags = frags_per_round;
            hz.start(m_sequencer, this);
        end
    endtask
endclass

class rop_fwd_directed_seq extends rop_base_seq;
    `uvm_object_utils(rop_fwd_directed_seq)

    function new(string name = "rop_fwd_directed_seq");
        super.new(name);
    endfunction

    // For each distance d = 1..5, each order (second farther / equal / closer)
    // and each way of making distance (idle cycles / filler fragments):
    // write A to a fresh cleared pixel, then send B to it d cycles later.
    // Fillers use z = 0xFFFF on other pixels, so they can never write.
    task body();
        int unsigned col = 0;
        for (int d = 1; d <= 5; d++)
            for (int order = 0; order < 3; order++)
                for (int fill = 0; fill < 2; fill++) begin
                    int unsigned fx = 10 + col;
                    int unsigned fy = 100 + d;
                    bit [15:0]   a  = 16'd1000;
                    bit [15:0]   b  = (order == 0) ? 16'd2000 : (order == 1) ? 16'd1000 : 16'd500;
                    col++;
                    send_frag(fx, fy, a);
                    if (fill) begin
                        repeat (d - 1) send_frag(200, 200, 16'hFFFF);
                        send_frag(fx, fy, b);
                    end else begin
                        send_frag(fx, fy, b, d - 1);
                    end
                end
    endtask
endclass
